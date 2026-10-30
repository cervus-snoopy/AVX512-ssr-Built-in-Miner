// AVX512 SSr — Miner Keccak-256 paralel cepat untuk pencarian nonce.
// File ini adalah bagian dari produk AVX512 SSr.
//
// avx512_miner.c — Miner PRODUKSI (AVX512 8-way Keccak-256).
//
// Pengganti yang lebih cepat untuk solver skalar pada pencarian nonce.
// Core permutasi Keccak-f1600 AVX512 disalin verbatim dari harness test
// ~/workspace/pod/avx512_test.c (terbukti benar: 24/24 lane cocok dengan
// keccak Python). Yang baru di sini adalah bungkus pencariannya:
//   - loop pencarian range [start, end) dengan target N leading zero bits,
//   - precompute blok konstan (blok 136-byte penuh yang seluruhnya di dalam
//     prefix) — dihitung SEKALI, dipakai ulang semua nonce,
//   - multi-thread (range dibagi per thread, kelipatan 8),
//   - berhenti seketika saat ketemu, cetak JSON ke stdout,
//   - progress berkala + file checkpoint ke stderr/file (untuk resume).
//
// FORMAT (TIDAK BOLEH DILANGGAR): pesan = prefix ASCII + nonce desimal ASCII.
// Byte order little-endian per word 8 byte, TANPA byte-swap. Konvensi
// Konvensi byte-swap lain tidak berlaku di sini.
//
// Usage:
//   ./avx512_miner "<prefix>" <bits> <start> <end> [threads]
//       [--checkpoint FILE] [--resume] [--progress SECS]
//   ./avx512_miner --digest "<prefix>" <nonce> [nonce2 ... nonce8]
//       (cetak digest JSON per nonce, untuk verifikasi; maks 8 nonce/batch)
//
// Ketemu -> stdout: {"nonce": "...", "hash": "0x..."} dan exit 0.
// Tidak ketemu -> stderr saja, exit 1. Progress/checkpoint -> stderr/file.
// Murni komputasi lokal: tidak ada jaringan, tidak ada wallet, tidak ada
// pengiriman hasil ke pihak mana pun.
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static const uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};
static const int R[25] = {
    0, 1, 62, 28, 27,
    36, 44, 6, 55, 20,
    3, 10, 43, 25, 39,
    41, 45, 15, 21, 8,
    18, 2, 61, 56, 14
};

static __m512i ROTV[25];
static __m512i ROT1V;
static void init_rotvecs(void) {
    for (int i = 0; i < 25; i++) ROTV[i] = _mm512_set1_epi64((long long)R[i]);
    ROT1V = _mm512_set1_epi64(1);
}
static inline __m512i rolv(__m512i v, __m512i rvec) {
    return _mm512_rolv_epi64(v, rvec);
}

// Keccak-f1600 vektor 8-way — verbatim dari avx512_test.c (terbukti benar).
static void keccak_f1600_vec(__m512i st[25]) {
    for (int round = 0; round < 24; round++) {
        __m512i C[5], D[5], B[25];
        for (int x = 0; x < 5; x++)
            C[x] = _mm512_xor_epi64(_mm512_xor_epi64(_mm512_xor_epi64(_mm512_xor_epi64(
                        st[x], st[x + 5]), st[x + 10]), st[x + 15]), st[x + 20]);
        for (int x = 0; x < 5; x++)
            D[x] = _mm512_xor_epi64(C[(x + 4) % 5], rolv(C[(x + 1) % 5], ROT1V));
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                st[x + 5 * y] = _mm512_xor_epi64(st[x + 5 * y], D[x]);
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                B[y + 5 * ((2 * x + 3 * y) % 5)] = rolv(st[x + 5 * y], ROTV[x + 5 * y]);
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                st[x + 5 * y] = _mm512_xor_epi64(B[x + 5 * y],
                    _mm512_andnot_epi64(B[(x + 1) % 5 + 5 * y], B[(x + 2) % 5 + 5 * y]));
        st[0] = _mm512_xor_epi64(st[0], _mm512_set1_epi64((long long)RC[round]));
    }
}

static int u64toa(uint64_t v, char *buf) {
    char tmp[20];
    int n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

// State awal setelah menyerap blok-blok konstan prefix (blok 136-byte penuh
// yang seluruhnya di dalam prefix). Dihitung SEKALI per proses.
static __m512i g_st0[25];
static const char *g_prefix;
static size_t g_plen;
static int g_const_blocks;   // floor(plen / 136)
static int g_tail_len;       // plen - const_blocks*136  (0..135)

static void init_const_state(void) {
    for (int i = 0; i < 25; i++) g_st0[i] = _mm512_setzero_si512();
    for (int c = 0; c < g_const_blocks; c++) {
        for (int i = 0; i < 17; i++) {
            uint64_t w = 0;
            // Blok konstan: byte-nya sama untuk semua lane -> broadcast.
            memcpy(&w, g_prefix + c * 136 + i * 8, 8);
            g_st0[i] = _mm512_xor_epi64(g_st0[i], _mm512_set1_epi64((long long)w));
        }
        keccak_f1600_vec(g_st0);
    }
}

// Hash 8 pesan (tail prefix + nonce desimal) -> out[8][32].
// tail = prefix setelah blok konstan (g_tail_len byte, < 136).
// Lane dengan panjang nonce berbeda (mis. 9 vs 10 digit) boleh beda jumlah
// blok: tiap lane di-squeeze tepat setelah blok terakhirnya sendiri.
static void keccak_batch8(const unsigned char *tail, const uint64_t nonces[8],
                          unsigned char out[8][32]) {
    unsigned char rem[8][256];
    int rem_len[8], rblocks[8], maxrblocks = 0;
    for (int h = 0; h < 8; h++) {
        char num[32];
        int nlen = u64toa(nonces[h], num);
        memcpy(rem[h], tail, (size_t)g_tail_len);
        memcpy(rem[h] + g_tail_len, num, (size_t)nlen);
        rem_len[h] = g_tail_len + nlen;
        int total = g_const_blocks * 136 + rem_len[h];
        rblocks[h] = total / 136 + 1 - g_const_blocks;
        if (rblocks[h] > maxrblocks) maxrblocks = rblocks[h];
    }
    __m512i st[25];
    for (int i = 0; i < 25; i++) st[i] = g_st0[i];
    int done[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int b = 0; b < maxrblocks; b++) {
        // Bangun blok per lane SEKALI per blok (bukan per word).
        unsigned char blk[8][136];
        for (int h = 0; h < 8; h++) {
            memset(blk[h], 0, 136);
            int base = b * 136;
            int seg = rem_len[h] - base;
            if (seg > 136) seg = 136;
            if (seg > 0) memcpy(blk[h], rem[h] + base, (size_t)seg);
            if (b == rblocks[h] - 1) {
                int p = rem_len[h] - base; // 0..135
                blk[h][p] ^= 0x01;
                blk[h][135] ^= 0x80;
            }
        }
        for (int i = 0; i < 17; i++) {
            uint64_t w[8];
            for (int h = 0; h < 8; h++) memcpy(&w[h], blk[h] + i * 8, 8);
            st[i] = _mm512_xor_epi64(st[i],
                _mm512_set_epi64((long long)w[7], (long long)w[6], (long long)w[5],
                                 (long long)w[4], (long long)w[3], (long long)w[2],
                                 (long long)w[1], (long long)w[0]));
        }
        keccak_f1600_vec(st);
        uint64_t lanes[4][8];
        for (int i = 0; i < 4; i++) _mm512_storeu_si512(lanes[i], st[i]);
        for (int h = 0; h < 8; h++) {
            if (!done[h] && b == rblocks[h] - 1) {
                for (int i = 0; i < 4; i++)
                    for (int k = 0; k < 8; k++)
                        out[h][i * 8 + k] = (unsigned char)(lanes[i][h] >> (8 * k));
                done[h] = 1;
            }
        }
    }
}

// Hitung leading zero bits digest (digest dibaca big-endian: byte pertama
// paling signifikan). Mengembalikan 0..256.
static int leading_zeros(const unsigned char h[32]) {
    int n = 0;
    for (int i = 0; i < 32; i++) {
        if (h[i] == 0) { n += 8; continue; }
        return n + __builtin_clz(h[i]) - 24;
    }
    return n;
}

static void print_json(uint64_t nonce, const unsigned char h[32]) {
    printf("{\"nonce\": \"%llu\", \"hash\": \"0x", (unsigned long long)nonce);
    for (int i = 0; i < 32; i++) printf("%02x", h[i]);
    printf("\"}\n");
    fflush(stdout);
}

// ---- pencarian ----

static int g_bits;
static _Atomic int g_found = 0;
static _Atomic uint64_t g_tried = 0;
static pthread_mutex_t g_result_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_result_nonce = 0;
static unsigned char g_result_hash[32];
static _Atomic uint64_t *g_frontier; // per thread: nonce berikutnya yang diproses
static int g_nthreads;

typedef struct { int tid; uint64_t start; uint64_t end; } thread_arg;

static void *worker(void *argp) {
    thread_arg *a = (thread_arg *)argp;
    uint64_t next = a->start;
    atomic_store(&g_frontier[a->tid], next);
    unsigned char tail[256];
    memcpy(tail, g_prefix + g_const_blocks * 136, (size_t)g_tail_len);
    while (next < a->end && !atomic_load(&g_found)) {
        uint64_t nonces[8];
        for (int j = 0; j < 8; j++) {
            uint64_t n = next + (uint64_t)j;
            nonces[j] = (n < a->end) ? n : next; // pad batch parsial dgn duplikat
        }
        unsigned char out[8][32];
        keccak_batch8(tail, nonces, out);
        for (int j = 0; j < 8 && !atomic_load(&g_found); j++) {
            if (nonces[j] >= a->end) continue;
            if (leading_zeros(out[j]) >= g_bits) {
                pthread_mutex_lock(&g_result_mu);
                if (!atomic_load(&g_found)) {
                    g_result_nonce = nonces[j];
                    memcpy(g_result_hash, out[j], 32);
                    atomic_store(&g_found, 1);
                }
                pthread_mutex_unlock(&g_result_mu);
                break;
            }
        }
        atomic_fetch_add(&g_tried, 8);
        next += 8;
        if (next > a->end) next = a->end;
        atomic_store(&g_frontier[a->tid], next);
    }
    return NULL;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void write_checkpoint(const char *path) {
    if (!path) return;
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "checkpoint: tidak bisa tulis %s\n", tmp); return; }
    for (int t = 0; t < g_nthreads; t++)
        fprintf(f, "thread %d %llu\n", t,
                (unsigned long long)atomic_load(&g_frontier[t]));
    fclose(f);
    rename(tmp, path);
}

// Muat checkpoint: kembalikan 1 bila ada baris valid dibaca (start tiap
// thread di-override ke nilai checkpoint, dijepit ke chunk-nya sendiri).
static int load_checkpoint(const char *path, thread_arg *args) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int got = 0;
    int tid;
    unsigned long long n;
    // Format diawali spasi: fscanf tidak melewati whitespace sebelum literal
    // "thread", jadi tanpa spasi hanya baris pertama yang terbaca.
    while (fscanf(f, " thread %d %llu", &tid, &n) == 2) {
        if (tid >= 0 && tid < g_nthreads) {
            uint64_t v = (uint64_t)n;
            if (v > args[tid].start && v < args[tid].end) {
                args[tid].start = v;
                got = 1;
            }
        }
    }
    fclose(f);
    return got;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s \"<prefix>\" <bits> <start> <end> [threads]\n"
        "          [--checkpoint FILE] [--resume] [--progress SECS]\n"
        "       %s --digest \"<prefix>\" <nonce> [nonce2 ... nonce8]\n",
        prog, prog);
}

int main(int argc, char **argv) {
    init_rotvecs();
    if (argc >= 2 && !strcmp(argv[1], "--digest")) {
        if (argc < 4) { usage(argv[0]); return 2; }
        g_prefix = argv[2];
        g_plen = strlen(g_prefix);
        g_const_blocks = (int)(g_plen / 136);
        g_tail_len = (int)(g_plen - (size_t)g_const_blocks * 136);
        init_const_state();
        int count = argc - 3;
        if (count > 8) { fprintf(stderr, "--digest: maks 8 nonce per batch\n"); return 2; }
        uint64_t nonces[8];
        for (int j = 0; j < 8; j++) {
            int idx = (j < count) ? j : 0;
            nonces[j] = strtoull(argv[3 + idx], NULL, 10);
        }
        unsigned char tail[256];
        memcpy(tail, g_prefix + g_const_blocks * 136, (size_t)g_tail_len);
        unsigned char out[8][32];
        keccak_batch8(tail, nonces, out);
        for (int j = 0; j < count; j++) print_json(nonces[j], out[j]);
        return 0;
    }
    if (argc < 5) { usage(argv[0]); return 2; }
    g_prefix = argv[1];
    g_plen = strlen(g_prefix);
    g_bits = atoi(argv[2]);
    uint64_t start = strtoull(argv[3], NULL, 10);
    uint64_t end = strtoull(argv[4], NULL, 10);
    int argi = 5;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    g_nthreads = (ncpu > 0) ? (int)ncpu : 2;
    if (argi < argc && argv[argi][0] != '-') {
        g_nthreads = atoi(argv[argi]);
        argi++;
    }
    const char *ckpt = NULL;
    int resume = 0;
    int progress_secs = 30;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "--checkpoint") && argi + 1 < argc) ckpt = argv[++argi];
        else if (!strcmp(argv[argi], "--resume")) resume = 1;
        else if (!strcmp(argv[argi], "--progress") && argi + 1 < argc) progress_secs = atoi(argv[++argi]);
        else { fprintf(stderr, "arg tidak dikenal: %s\n", argv[argi]); usage(argv[0]); return 2; }
    }
    if (g_bits < 1 || g_bits > 256) { fprintf(stderr, "bits harus 1..256\n"); return 2; }
    if (g_nthreads < 1) g_nthreads = 1;
    if (progress_secs < 1) progress_secs = 1;
    if (start >= end) { fprintf(stderr, "start harus < end\n"); return 2; }

    g_const_blocks = (int)(g_plen / 136);
    g_tail_len = (int)(g_plen - (size_t)g_const_blocks * 136);
    init_const_state();
    fprintf(stderr, "mencari: bits=%d range=[%llu, %llu) threads=%d "
                    "prefix_len=%zu const_blocks=%d tail_len=%d\n",
            g_bits, (unsigned long long)start, (unsigned long long)end,
            g_nthreads, g_plen, g_const_blocks, g_tail_len);

    // Bagi range per thread, kelipatan 8 (selaras batch).
    uint64_t total = end - start;
    uint64_t chunk = (total + (uint64_t)g_nthreads - 1) / (uint64_t)g_nthreads;
    chunk = (chunk + 7) / 8 * 8;
    thread_arg *args = calloc((size_t)g_nthreads, sizeof(thread_arg));
    g_frontier = calloc((size_t)g_nthreads, sizeof(_Atomic uint64_t));
    for (int t = 0; t < g_nthreads; t++) {
        uint64_t ts = start + (uint64_t)t * chunk;
        uint64_t te = ts + chunk;
        if (te > end) te = end;
        if (ts > end) ts = end;
        args[t].tid = t;
        args[t].start = ts;
        args[t].end = te;
        atomic_init(&g_frontier[t], ts);
    }
    if (resume && ckpt) {
        if (load_checkpoint(ckpt, args))
            fprintf(stderr, "resume: start thread di-override dari %s\n", ckpt);
        else
            fprintf(stderr, "resume: tidak ada checkpoint valid di %s, mulai dari awal\n", ckpt);
        for (int t = 0; t < g_nthreads; t++)
            atomic_store(&g_frontier[t], args[t].start);
    }

    pthread_t *ths = malloc(sizeof(pthread_t) * (size_t)g_nthreads);
    double t0 = now_s();
    for (int t = 0; t < g_nthreads; t++) pthread_create(&ths[t], NULL, worker, &args[t]);
    // Progress berkala di thread utama sampai semua thread selesai/ketemu.
    // Granularitas cek 100ms agar run pendek tidak tertahan tidur 1 detik.
    int alive = 1;
    while (alive) {
        // Tidur progress_secs detik, tapi bangun lebih awal bila sudah selesai.
        for (int s = 0; s < progress_secs * 10; s++) {
            usleep(100000);
            alive = 0;
            for (int t = 0; t < g_nthreads; t++) {
                // Thread dianggap hidup bila frontier-nya belum mencapai end
                // dan belum ada temuan; join di bawah memastikan akhir.
                if (!atomic_load(&g_found) &&
                    atomic_load(&g_frontier[t]) < args[t].end) { alive = 1; break; }
            }
            if (!alive) break;
        }
        if (alive) {
            double el = now_s() - t0;
            uint64_t done = atomic_load(&g_tried);
            fprintf(stderr, "[progress] %.0fs: %llu hashes, %.2f M/s, frontier:",
                    el, (unsigned long long)done, (done / el) / 1e6);
            for (int t = 0; t < g_nthreads; t++)
                fprintf(stderr, " t%d@%llu", t, (unsigned long long)atomic_load(&g_frontier[t]));
            fprintf(stderr, "\n");
            write_checkpoint(ckpt);
        }
    }
    for (int t = 0; t < g_nthreads; t++) pthread_join(ths[t], NULL);
    double el = now_s() - t0;
    uint64_t done = atomic_load(&g_tried);
    fprintf(stderr, "[final] %.1fs: %llu hashes, %.2f M/s\n",
            el, (unsigned long long)done, (done / (el > 0 ? el : 1)) / 1e6);
    write_checkpoint(ckpt);
    if (atomic_load(&g_found)) {
        print_json(g_result_nonce, g_result_hash);
        return 0;
    }
    fprintf(stderr, "tidak ketemu di [%llu, %llu)\n",
            (unsigned long long)start, (unsigned long long)end);
    return 1;
}
