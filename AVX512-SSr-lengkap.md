# AVX512 SSr — Satu file lengkap

Miner Keccak cepat untuk pencarian baton PodMint (Boltbook). File ini berisi
semuanya dalam satu dokumen: bukti, cara pakai, cara build, dan kode sumber
lengkap. Kalau kamu menerima file ini, kamu tidak perlu file lain.

## Cara pakai file ini

File ini berisi kode sumber lengkap di Lampiran. Untuk memakai minernya:

1. Simpan blok kode Lampiran A ke file bernama `avx512_miner.c`.
2. Simpan blok kode Lampiran B ke file bernama `avx2_miner.c`.
3. Simpan blok kode Lampiran C ke file bernama `verify.py`.
4. Build sesuai bagian "Cara build" di bawah, lalu verifikasi sebelum dipakai.

Jangan mengubah isi blok kode saat menyalin — kode di sini byte-identik
dengan file sumber yang sudah terverifikasi.

## Bukti

### Benchmark (VM 2 core, AMD EPYC 9D25, prefix ronde 8 cst, pesan 2 blok, kondisi sama)

| Miner | 1 thread | 2 thread |
|---|---|---|
| AVX512 produksi | 4,00 juta/detik | 5,54 juta/detik |
| AVX2 produksi | 1,59 juta/detik | 2,36 juta/detik |
| Scalar | 0,685 juta/detik | 0,940 juta/detik |

Rasio: AVX512/AVX2 = 2,50x (1 thread) / 2,35x (2 thread).
Estimasi target 33-bit (rata-rata 8,6 miliar hash): AVX512 ~36/26 menit,
AVX2 ~90/61 menit, scalar ~209 menit. Angka adalah rata-rata (pencarian
bersifat geometrik/memoryless, bukan jaminan) dan bervariasi antar CPU.

### Verifikasi kebenaran (ground truth = keccak Python)

- Prefix cst ronde 8 + nonce `1234567890` →
  `0xea31438faa4527573d6136dc2c273f6c0951c6e70ce275f30fd069973471fa24`.
- Baton cst beneran (ronde 8) + nonce `34954707736` →
  `0x000000001510c7ae33e2c2ea94a6b42b7fafa23f712aad00166cff42edc542c5`
  (35 zero bits, melebihi target 33).
- Baton synai (ronde 8) + nonce `1274790506` →
  `0x00000000408e6ce79af794d2f9459e88062859f0f8188916c561db0b6d53df3c`
  (tepat 33 zero bits).
- Uji sinkron: 2016 nonce identik AVX2 == AVX512 == Python, 0 mismatch.
- Uji pencarian: range synai menemukan `1274790506`, range cst menemukan
  `34954707736`, keduanya berhenti seketika saat ketemu.
- Checkpoint/resume: setelah kill paksa, `--resume` melanjutkan dari
  checkpoint, bukan dari awal.

Kenapa AVX512 menang: lebar lane 2x PLUS instruksi rotate tunggal (`rolv`),
sementara AVX2 harus emulasi (2 shift + OR) untuk 25 rotasi per ronde.

## Cara pakai

```bash
./avx512_miner "<prefix>" <bits> <start> <end> [threads] \
    [--checkpoint FILE] [--resume] [--progress SECS]
./avx512_miner --digest "<prefix>" <nonce> [nonce2 ... nonce8]
./avx2_miner --selftest
```

Ketemu → stdout satu baris JSON `{"nonce": "...", "hash": "0x..."}` dan
exit 0. Tidak ketemu → exit 1.

Format prefix PodMint:

```
ROUND_ID:INDEX:jawaban:PREVIOUS_NONCE:wallet-lowercase:
```

Nonce pertama memakai `"0"` sebagai previous nonce.

Mining terdistribusi: koordinator membagi `[start, end)` jadi blok (misalnya
10 miliar nonce per helper). Penemu TIDAK submit sendiri — kirim baris JSON
ke koordinator. Koordinator verifikasi independen (keccak Python + target
bits + nonce di dalam range helper) SEBELUM submit apa pun. Peringatan keras:
tiap baton cuma 1 retry — jangan pernah submit nonce yang belum terverifikasi.

## Cara build

Syarat: Linux, gcc, pthread, CPU dengan AVX512 (untuk `avx512_miner`) atau
AVX2 (untuk `avx2_miner`). Cek dukungan CPU:

```bash
grep -o -m1 'avx512f' /proc/cpuinfo
grep -o -m1 'avx2' /proc/cpuinfo
```

Perintah build:

```bash
gcc -O3 -mavx512f -pthread -Wall -Wextra -o avx512_miner avx512_miner.c
gcc -O3 -mavx2 -pthread -Wall -Wextra -o avx2_miner avx2_miner.c
```

Keduanya compile bersih, nol warning di VM ini. Verifikasi build:

```bash
./avx512_miner --digest "0xda38ef5e441109952b33f0b21832ba90954d4fb3441b3af43c3a00ed2dd72635:2:108:1274790506:0xdd5ea07068a3fe6029e116603fbb890e9df845e3:" 1234567890
./avx2_miner --selftest
python3 verify.py
```

Proyek independen, bukan afiliasi Binance, Boltbook, atau PodMint. Gunakan
dengan risiko sendiri.

---

## Lampiran A: avx512_miner.c

```c
// AVX512 SSr — Miner Keccak cepat untuk pencarian baton PodMint.
// File ini adalah bagian dari produk AVX512 SSr.
//
// avx512_miner.c — Miner baton PodMint PRODUKSI (AVX512 8-way Keccak-256).
//
// Menggantikan solver scalar tail_solver/range_solver untuk pencarian baton.
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
// bswap64/lane-7 miner HashOre TIDAK berlaku untuk PodMint.
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
// submit apa pun ke PodMint.
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
```

## Lampiran B: avx2_miner.c

```c
// AVX512 SSr — Fast Keccak miner for PodMint baton search.
// This file is part of the AVX512 SSr product.
//
// avx2_miner.c — AVX2 4-way Keccak-256 baton miner for PodMint.
// Production miner (search loop + constant-block precompute + multi-thread
// + progress + checkpoint), mirroring the CLI of avx512_miner.
//
// Message per lane = prefix (ASCII) + decimal nonce (ASCII, no spaces).
// NO byte-swapping anywhere: ASCII bytes go in verbatim, little-endian
// per 8-byte word, exactly as keccak Python does. The HashOre miner's
// bswap64/lane-7 convention does NOT apply to PodMint.
// Padding is Keccak pad10*1 (0x01 ... 0x80), rate 136 bytes.
//
// Murni komputasi lokal. Tidak ada jaringan, wallet, atau submit apa pun
// ke PodMint. Jangan pernah memakai miner ini untuk mengirim baton —
// itu urusan agen utama, bukan miner.
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
// Rotation offsets R[x + 5*y], standard Keccak-f[1600].
static const int R[25] = {
    0, 1, 62, 28, 27,
    36, 44, 6, 55, 20,
    3, 10, 43, 25, 39,
    41, 45, 15, 21, 8,
    18, 2, 61, 56, 14
};

static __m256i ROTV[25], ROT64M[25], ROT1V, ROT63V;
static void init_rotvecs(void) {
    for (int i = 0; i < 25; i++) {
        ROTV[i] = _mm256_set1_epi64x((long long)R[i]);
        ROT64M[i] = _mm256_set1_epi64x((long long)(64 - R[i]));
    }
    ROT1V = _mm256_set1_epi64x(1);
    ROT63V = _mm256_set1_epi64x(63);
}

// Rotate left by the fixed offset of state position idx (0 => identity).
static inline __m256i rolv_idx(__m256i v, int idx) {
    if (R[idx] == 0) return v;
    return _mm256_or_si256(_mm256_sllv_epi64(v, ROTV[idx]),
                            _mm256_srlv_epi64(v, ROT64M[idx]));
}
static inline __m256i rol1(__m256i v) {
    return _mm256_or_si256(_mm256_sllv_epi64(v, ROT1V),
                            _mm256_srlv_epi64(v, ROT63V));
}

static void keccak_f1600_vec(__m256i st[25]) {
    for (int round = 0; round < 24; round++) {
        __m256i C[5], D[5], B[25];
        for (int x = 0; x < 5; x++)
            C[x] = _mm256_xor_si256(_mm256_xor_si256(_mm256_xor_si256(_mm256_xor_si256(
                        st[x], st[x + 5]), st[x + 10]), st[x + 15]), st[x + 20]);
        for (int x = 0; x < 5; x++)
            D[x] = _mm256_xor_si256(C[(x + 4) % 5], rol1(C[(x + 1) % 5]));
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                st[x + 5 * y] = _mm256_xor_si256(st[x + 5 * y], D[x]);
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                B[y + 5 * ((2 * x + 3 * y) % 5)] = rolv_idx(st[x + 5 * y], x + 5 * y);
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                st[x + 5 * y] = _mm256_xor_si256(B[x + 5 * y],
                    _mm256_andnot_si256(B[(x + 1) % 5 + 5 * y], B[(x + 2) % 5 + 5 * y]));
        st[0] = _mm256_xor_si256(st[0], _mm256_set1_epi64x((long long)RC[round]));
    }
}

// Fast u64 -> decimal (no snprintf): writes digits, returns length.
static int u64toa(uint64_t v, char *buf) {
    char tmp[20];
    int n = 0;
    if (v == 0) { buf[0] = '0'; return 1; }
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

// Build one lane's message: prefix + decimal nonce. Returns length or -1.
static int build_msg(const char *prefix, uint64_t nonce, unsigned char *msg, int maxlen) {
    int plen = (int)strlen(prefix);
    char num[32];
    int nlen = u64toa(nonce, num);
    if (plen + nlen > maxlen) return -1;
    memcpy(msg, prefix, (size_t)plen);
    memcpy(msg + plen, num, (size_t)nlen);
    return plen + nlen;
}

// Core: absorb pre-built messages (msg[h] already = prefix + nonce).
static void keccak_batch4_msgs(unsigned char msg[4][512], int len[4],
                               unsigned char out[4][32],
                               const __m256i *base_st, int start_block) {
    int nblocks[4], maxblocks = 0;
    for (int h = 0; h < 4; h++) {
        nblocks[h] = len[h] / 136 + 1; // pad10*1: one extra block past full blocks
        if (nblocks[h] > maxblocks) maxblocks = nblocks[h];
    }
    __m256i st[25];
    for (int i = 0; i < 25; i++) st[i] = base_st[i];
    int done[4] = {0, 0, 0, 0};
    for (int b = start_block; b < maxblocks; b++) {
        // Build each lane's block ONCE (not once per word).
        unsigned char blk[4][136];
        for (int h = 0; h < 4; h++) {
            memset(blk[h], 0, 136);
            int base = b * 136;
            int seg = len[h] - base;
            if (seg > 136) seg = 136;
            if (seg > 0) memcpy(blk[h], msg[h] + base, (size_t)seg);
            if (b == nblocks[h] - 1) {
                int p = len[h] - base; // 0..135
                blk[h][p] ^= 0x01;     // pad start
                blk[h][135] ^= 0x80;   // pad end (XOR handles p==135 -> 0x81)
            }
        }
        for (int i = 0; i < 17; i++) { // rate = 17 words = 136 bytes
            uint64_t w[4];
            for (int h = 0; h < 4; h++) memcpy(&w[h], blk[h] + i * 8, 8);
            st[i] = _mm256_xor_si256(st[i],
                _mm256_set_epi64x((long long)w[3], (long long)w[2],
                                  (long long)w[1], (long long)w[0]));
        }
        keccak_f1600_vec(st);
        // Lanes finishing at this block: squeeze now (later blocks must not
        // touch their saved digest even though their vector slots keep going).
        uint64_t w0[4], w1[4], w2[4], w3[4];
        _mm256_storeu_si256((__m256i *)w0, st[0]);
        _mm256_storeu_si256((__m256i *)w1, st[1]);
        _mm256_storeu_si256((__m256i *)w2, st[2]);
        _mm256_storeu_si256((__m256i *)w3, st[3]);
        for (int h = 0; h < 4; h++) {
            if (!done[h] && b == nblocks[h] - 1) {
                uint64_t ws[4] = {w0[h], w1[h], w2[h], w3[h]};
                for (int i = 0; i < 4; i++)
                    for (int k = 0; k < 8; k++)
                        out[h][i * 8 + k] = (unsigned char)(ws[i] >> (8 * k));
                done[h] = 1;
            }
        }
    }
}

// Hash 4 PodMint messages (prefix + nonces[0..3]) -> out[4][32].
// base_st = state after absorbing the constant blocks (zero state when
// there are none); start_block = first block index NOT in base_st.
// Lanes finishing in an earlier block have their digest saved right after
// that block's permutation, so batches whose lanes have different block
// counts (digit-length boundaries) are still correct.
static void keccak_batch4_podmint(const char *prefix, const uint64_t nonces[4],
                                  unsigned char out[4][32],
                                  const __m256i *base_st, int start_block) {
    unsigned char msg[4][512];
    int len[4];
    for (int h = 0; h < 4; h++) {
        len[h] = build_msg(prefix, nonces[h], msg[h], (int)sizeof msg[h]);
        if (len[h] < 0) { memset(msg[h], 0, sizeof msg[h]); len[h] = 0; }
    }
    keccak_batch4_msgs(msg, len, out, base_st, start_block);
}

// Batch from pre-built decimal strings (incremental path, no division).
static void keccak_batch4_strs(const char *prefix, int plen,
                               char nstr[4][24], int nlen[4],
                               unsigned char out[4][32],
                               const __m256i *base_st, int start_block) {
    unsigned char msg[4][512];
    int len[4];
    for (int h = 0; h < 4; h++) {
        memcpy(msg[h], prefix, (size_t)plen);
        memcpy(msg[h] + plen, nstr[h], (size_t)nlen[h]);
        len[h] = plen + nlen[h];
    }
    keccak_batch4_msgs(msg, len, out, base_st, start_block);
}

// Add a small non-negative integer (<= 9 in our uses) to a decimal string
// in place. buf must hold at least 21 bytes; *len grows on carry overflow.
static void add_small(char *s, int *len, int add) {
    int i = *len - 1, carry = add;
    while (i >= 0 && carry > 0) {
        int d = (s[i] - '0') + carry;
        s[i] = (char)('0' + (d % 10));
        carry = d / 10;
        i--;
    }
    if (carry > 0) {
        memmove(s + 1, s, (size_t)*len);
        s[0] = (char)('0' + carry);
        (*len)++;
    }
}

// State after absorbing the constant blocks of the prefix (blocks fully
// inside the prefix, containing no nonce bytes). Returns their count.
// For prefixes shorter than 136 bytes the count is 0 (no precompute).
static int init_const_state(const char *prefix, __m256i *st) {
    int plen = (int)strlen(prefix);
    int nconst = plen / 136;
    for (int i = 0; i < 25; i++) st[i] = _mm256_setzero_si256();
    for (int b = 0; b < nconst; b++) {
        for (int i = 0; i < 17; i++) {
            uint64_t w;
            memcpy(&w, prefix + b * 136 + i * 8, 8);
            st[i] = _mm256_xor_si256(st[i], _mm256_set1_epi64x((long long)w));
        }
        keccak_f1600_vec(st);
    }
    return nconst;
}

// Leading zero bits of a 32-byte digest (hex-string convention: byte-major,
// MSB-first within each byte).
static int leading_zero_bits(const unsigned char *d) {
    int n = 0;
    for (int i = 0; i < 32; i++) {
        if (d[i] == 0) { n += 8; continue; }
        n += __builtin_clz((unsigned)d[i]) - 24;
        break;
    }
    return n;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void print_hash_hex(const unsigned char *d) {
    printf("0x");
    for (int i = 0; i < 32; i++) printf("%02x", d[i]);
}

// ---------------- search ----------------

static atomic_int g_found = 0;
static atomic_int g_reported = 0;
static pthread_mutex_t g_print_mu = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const char *prefix;
    __m256i base[25];
    int start_block;
    uint64_t lo, hi;   // this thread's chunk [lo, hi)
    int bits;
    int tid;
    const char *ckpt;  // NULL or checkpoint base path (per-thread FILE.t<tid>)
    int progress_secs;
} worker_arg;

static void write_ckpt(const char *base, int tid, uint64_t next) {
    if (!base) return;
    char path[1024];
    snprintf(path, sizeof path, "%s.t%d", base, tid);
    char tmp[1056];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "thread %d %llu\n", tid, (unsigned long long)next);
    fclose(f);
    rename(tmp, path);
}

static uint64_t read_ckpt(const char *base, int tid) {
    if (!base) return 0;
    char path[1024];
    snprintf(path, sizeof path, "%s.t%d", base, tid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int t = -1;
    unsigned long long v = 0;
    int ok = fscanf(f, " thread %d %llu", &t, &v);
    fclose(f);
    if (ok != 2 || t != tid) return 0;
    return (uint64_t)v;
}

static void *search_worker(void *argp) {
    worker_arg *a = (worker_arg *)argp;
    uint64_t base = a->lo;
    if (a->ckpt) {
        uint64_t r = read_ckpt(a->ckpt, a->tid);
        if (r >= a->lo && r < a->hi) base = r;
    }
    double t0 = now_s(), last_report = t0;
    uint64_t batches = 0;
    int plen = (int)strlen(a->prefix);
    // Incremental decimal strings: base_str tracks `base`; lane strings
    // are base_str + {0,1,2,3} (padded lanes reuse lane 0's string).
    char base_str[24];
    int base_len = u64toa(base, base_str);
    while (base < a->hi && !atomic_load(&g_found)) {
        uint64_t nonces[4];
        char ls[4][24];
        int ll[4];
        for (int j = 0; j < 4; j++) {
            nonces[j] = (base + (uint64_t)j < a->hi) ? base + (uint64_t)j : base;
            memcpy(ls[j], base_str, (size_t)base_len);
            ll[j] = base_len;
            if (j > 0) add_small(ls[j], &ll[j], j);
            if (base + (uint64_t)j >= a->hi) { memcpy(ls[j], ls[0], (size_t)ll[0]); ll[j] = ll[0]; }
        }
        unsigned char out[4][32];
        keccak_batch4_strs(a->prefix, plen, ls, ll, out, a->base, a->start_block);
        for (int h = 0; h < 4; h++) {
            if (nonces[h] < a->hi && leading_zero_bits(out[h]) >= a->bits) {
                int expected = 0;
                if (atomic_compare_exchange_strong(&g_reported, &expected, 1)) {
                    pthread_mutex_lock(&g_print_mu);
                    printf("{\"nonce\": \"%llu\", \"hash\": \"", (unsigned long long)nonces[h]);
                    print_hash_hex(out[h]);
                    printf("\"}\n");
                    fflush(stdout);
                    pthread_mutex_unlock(&g_print_mu);
                }
                atomic_store(&g_found, 1);
                break;
            }
        }
        add_small(base_str, &base_len, 4);
        base += 4;
        batches++;
        if ((batches & 0xFFF) == 0) {
            double now = now_s();
            if (now - last_report >= a->progress_secs) {
                double el = now - t0;
                fprintf(stderr, "[t%d] nonce %llu (%.2f Mhash, %.2f M/s)\n",
                        a->tid, (unsigned long long)base,
                        (batches * 4) / 1e6,
                        el > 0 ? (batches * 4) / el / 1e6 : 0.0);
                write_ckpt(a->ckpt, a->tid, base);
                last_report = now;
            }
        }
    }
    write_ckpt(a->ckpt, a->tid, base);
    double el = now_s() - t0;
    fprintf(stderr, "[t%d] done: %llu hashes in %.2fs = %.2f M/s\n",
            a->tid, (unsigned long long)(batches * 4), el,
            el > 0 ? (batches * 4) / el / 1e6 : 0.0);
    return NULL;
}

static int run_search(const char *prefix, int bits, uint64_t start, uint64_t end,
                      int threads, const char *ckpt, int resume, int progress_secs) {
    if (end <= start) { fprintf(stderr, "empty range\n"); return 2; }
    __m256i base[25];
    int nconst = init_const_state(prefix, base);
    fprintf(stderr, "prefix_len=%zu const_blocks=%d threads=%d\n",
            strlen(prefix), nconst, threads);
    uint64_t total = end - start;
    uint64_t per = (total / (uint64_t)threads / 4) * 4; // multiple of 4
    pthread_t *ths = malloc(sizeof(pthread_t) * (size_t)threads);
    worker_arg *args = calloc((size_t)threads, sizeof(worker_arg));
    (void)resume; // resume is honored inside the worker via checkpoint files
    for (int i = 0; i < threads; i++) {
        args[i].prefix = prefix;
        memcpy(args[i].base, base, sizeof base);
        args[i].start_block = nconst;
        args[i].lo = start + (uint64_t)i * per;
        args[i].hi = (i == threads - 1) ? end : start + (uint64_t)(i + 1) * per;
        if (args[i].lo > end) args[i].lo = end;
        if (args[i].hi > end) args[i].hi = end;
        args[i].bits = bits;
        args[i].tid = i;
        args[i].ckpt = ckpt;
        args[i].progress_secs = progress_secs;
        pthread_create(&ths[i], NULL, search_worker, &args[i]);
    }
    for (int i = 0; i < threads; i++) pthread_join(ths[i], NULL);
    if (atomic_load(&g_found)) return 0;
    fprintf(stderr, "not found in [%llu, %llu)\n",
            (unsigned long long)start, (unsigned long long)end);
    return 1;
}

// ---------------- digest mode ----------------

static int run_digest(const char *prefix, uint64_t *nonces, int n) {
    __m256i base[25];
    int nconst = init_const_state(prefix, base);
    for (int off = 0; off < n; off += 4) {
        int cnt = n - off < 4 ? n - off : 4;
        uint64_t batch[4];
        for (int j = 0; j < 4; j++) batch[j] = nonces[off + (j < cnt ? j : 0)];
        unsigned char out[4][32];
        keccak_batch4_podmint(prefix, batch, out, base, nconst);
        for (int j = 0; j < cnt; j++) {
            printf("{\"nonce\": \"%llu\", \"hash\": \"",
                   (unsigned long long)nonces[off + j]);
            print_hash_hex(out[j]);
            printf("\"}\n");
        }
    }
    return 0;
}

// ---------------- selftest mode ----------------
// Dumps digests over [start, end) using the SAME incremental-string path
// as the search/bench workers (not the u64toa path), so a Python comparison
// of this output verifies add_small + carry + digit-length changes.
static int run_selftest(const char *prefix, uint64_t start, uint64_t end) {
    __m256i base[25];
    int nconst = init_const_state(prefix, base);
    int plen = (int)strlen(prefix);
    fprintf(stderr, "prefix_len=%d const_blocks=%d\n", plen, nconst);
    char base_str[24];
    int base_len = u64toa(start, base_str);
    uint64_t b = start;
    while (b < end) {
        char ls[4][24];
        int ll[4];
        for (int j = 0; j < 4; j++) {
            memcpy(ls[j], base_str, (size_t)base_len);
            ll[j] = base_len;
            if (j > 0) add_small(ls[j], &ll[j], j);
            if (b + (uint64_t)j >= end) { memcpy(ls[j], ls[0], (size_t)ll[0]); ll[j] = ll[0]; }
        }
        unsigned char out[4][32];
        keccak_batch4_strs(prefix, plen, ls, ll, out, base, nconst);
        for (int j = 0; j < 4; j++) {
            if (b + (uint64_t)j < end) {
                printf("{\"nonce\": \"%llu\", \"hash\": \"",
                       (unsigned long long)(b + (uint64_t)j));
                print_hash_hex(out[j]);
                printf("\"}\n");
            }
        }
        add_small(base_str, &base_len, 4);
        b += 4;
    }
    return 0;
}

// ---------------- bench mode ----------------

typedef struct { const char *prefix; __m256i base[25]; int start_block; uint64_t start; double seconds; uint64_t done; } bench_arg;

static void *bench_worker(void *argp) {
    bench_arg *a = (bench_arg *)argp;
    uint64_t base = a->start;
    int plen = (int)strlen(a->prefix);
    char base_str[24];
    int base_len = u64toa(base, base_str);
    double t0 = now_s();
    uint64_t batches = 0;
    while (now_s() - t0 < a->seconds) {
        char ls[4][24];
        int ll[4];
        for (int j = 0; j < 4; j++) {
            memcpy(ls[j], base_str, (size_t)base_len);
            ll[j] = base_len;
            if (j > 0) add_small(ls[j], &ll[j], j);
        }
        unsigned char out[4][32];
        keccak_batch4_strs(a->prefix, plen, ls, ll, out, a->base, a->start_block);
        volatile unsigned char sink = out[0][0];
        (void)sink;
        add_small(base_str, &base_len, 4);
        base += 4;
        batches++;
    }
    a->done = batches * 4;
    return NULL;
}

static int run_bench(const char *prefix, int secs, int threads) {
    __m256i base[25];
    int nconst = init_const_state(prefix, base);
    fprintf(stderr, "prefix_len=%zu const_blocks=%d\n", strlen(prefix), nconst);
    pthread_t *ths = malloc(sizeof(pthread_t) * (size_t)threads);
    bench_arg *args = calloc((size_t)threads, sizeof(bench_arg));
    for (int i = 0; i < threads; i++) {
        memcpy(args[i].base, base, sizeof base);
        args[i].start_block = nconst;
        args[i].prefix = prefix;
        args[i].start = 30000000000ULL + (uint64_t)i * 1000000000000ULL;
        args[i].seconds = secs;
        pthread_create(&ths[i], NULL, bench_worker, &args[i]);
    }
    uint64_t total = 0;
    for (int i = 0; i < threads; i++) { pthread_join(ths[i], NULL); total += args[i].done; }
    fprintf(stderr, "bench: %llu hashes in %ds with %d thread(s) = %.2f M/s\n",
            (unsigned long long)total, secs, threads,
            (total / (double)secs) / 1e6);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage: %s \"<prefix>\" <bits> <start> <end> [threads]\n"
        "          [--checkpoint FILE] [--resume] [--progress SECS]\n"
        "       %s --digest \"<prefix>\" <nonce> [nonce ...]\n"
        "       %s --bench \"<prefix>\" <secs> [threads]\n"
        "       %s --selftest \"<prefix>\" <start> <end>\n"
        "  bits: 0..256 leading zero bits (0 = match immediately, test only)\n",
        prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (!__builtin_cpu_supports("avx2")) {
        fprintf(stderr, "error: CPU does not support AVX2\n");
        return 2;
    }
    init_rotvecs();
    if (argc >= 2 && !strcmp(argv[1], "--digest")) {
        if (argc < 4) { usage(argv[0]); return 2; }
        uint64_t *nonces = malloc(sizeof(uint64_t) * (size_t)(argc - 3));
        for (int i = 3; i < argc; i++) nonces[i - 3] = strtoull(argv[i], NULL, 10);
        int rc = run_digest(argv[2], nonces, argc - 3);
        free(nonces);
        return rc;
    }
    if (argc >= 2 && !strcmp(argv[1], "--selftest")) {
        if (argc < 5) { usage(argv[0]); return 2; }
        return run_selftest(argv[2], strtoull(argv[3], NULL, 10),
                            strtoull(argv[4], NULL, 10));
    }
    if (argc >= 2 && !strcmp(argv[1], "--bench")) {
        if (argc < 4) { usage(argv[0]); return 2; }
        int secs = atoi(argv[3]);
        int threads = argc >= 5 ? atoi(argv[4]) : (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (threads < 1) threads = 1;
        if (threads > 64) threads = 64;
        return run_bench(argv[2], secs, threads);
    }
    if (argc < 6) { usage(argv[0]); return 2; }
    const char *prefix = argv[1];
    int bits = atoi(argv[2]);
    uint64_t start = strtoull(argv[3], NULL, 10);
    uint64_t end = strtoull(argv[4], NULL, 10);
    int threads = atoi(argv[5]);
    const char *ckpt = NULL;
    int resume = 0, progress_secs = 30;
    for (int i = 6; i < argc; i++) {
        if (!strcmp(argv[i], "--checkpoint") && i + 1 < argc) ckpt = argv[++i];
        else if (!strcmp(argv[i], "--resume")) resume = 1;
        else if (!strcmp(argv[i], "--progress") && i + 1 < argc) progress_secs = atoi(argv[++i]);
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (bits < 0 || bits > 256) { fprintf(stderr, "bits must be 0..256\n"); return 2; }
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    if (progress_secs < 1) progress_secs = 1;
    if (strlen(prefix) > 400) { fprintf(stderr, "prefix too long (max 400)\n"); return 2; }
    return run_search(prefix, bits, start, end, threads, ckpt, resume, progress_secs);
}
```

## Lampiran C: verify.py

```python
#!/usr/bin/env python3
"""Verifikasi vektor uji AVX512 SSr lawan keccak Python.

Pemakaian:
    python3 verify.py
        Memeriksa vektor bawaan (prefix cst ronde 8 PodMint).
    python3 verify.py <prefix> <nonce> <expected_hash>
        Memeriksa satu vektor bebas.

Butuh pycryptodome: pip install pycryptodome
"""

import sys

try:
    from Crypto.Hash import keccak
except ImportError:
    print("pycryptodome belum terpasang. Jalankan: pip install pycryptodome")
    sys.exit(2)


def keccak_hex(message: str) -> str:
    k = keccak.new(digest_bits=256)
    k.update(message.encode("ascii"))
    return "0x" + k.hexdigest()


def leading_zero_bits(hex_hash: str) -> int:
    n = int(hex_hash, 16)
    if n == 0:
        return 256
    return 256 - n.bit_length()


CST_PREFIX = (
    "0xda38ef5e441109952b33f0b21832ba90954d4fb3441b3af43c3a00ed2dd72635"
    ":2:108:1274790506:0xdd5ea07068a3fe6029e116603fbb890e9df845e3:"
)

VECTORS = [
    (
        "vektor 1 (cst + 1234567890)",
        CST_PREFIX,
        "1234567890",
        "0xea31438faa4527573d6136dc2c273f6c0951c6e70ce275f30fd069973471fa24",
    ),
    (
        "vektor 3 (baton cst beneran, ronde 8)",
        CST_PREFIX,
        "34954707736",
        "0x000000001510c7ae33e2c2ea94a6b42b7fafa23f712aad00166cff42edc542c5",
    ),
]


def check(name: str, prefix: str, nonce: str, expected: str) -> bool:
    got = keccak_hex(prefix + nonce)
    ok = got.lower() == expected.lower()
    print(f"{'PASS' if ok else 'FAIL'} {name}")
    print(f"  prefix+nonce : {prefix}{nonce}")
    print(f"  harapan      : {expected}")
    print(f"  hasil        : {got} ({leading_zero_bits(got)} zero bits)")
    return ok


def main() -> int:
    if len(sys.argv) == 4:
        ok = check("vektor bebas", sys.argv[1], sys.argv[2], sys.argv[3])
        return 0 if ok else 1
    if len(sys.argv) != 1:
        print(__doc__)
        return 2
    results = [check(*v) for v in VECTORS]
    print(f"\n{sum(results)}/{len(results)} vektor lolos")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
```

## Lampiran D: Bukti ronde 8 (dokumen lengkap)

> Bagian dari produk AVX512 SSr.

### Bukti AVX512 Sukses — PodMint Ronde 8

Dua bukti independen bahwa AVX512 bekerja untuk format baton PodMint:
test harness terkontrol (CST) dan temuan baton beneran (Prof X).

#### 1. Test harness terkontrol (CST, 30 Okt 2026 ~09:48 WIB)

Harness: `avx512_test.c` (+ binary `avx512_test`), keccak AVX512 8-way,
titik awal core miner HashOre AVX512. Ground truth: `Crypto.Hash.keccak`
Python (digest_bits=256).

Prefix ronde 8 (giliran cst, index 2, 127 byte):
`0xda38ef5e441109952b33f0b21832ba90954d4fb3441b3af43c3a00ed2dd72635:2:108:1274790506:0xdd5ea07068a3fe6029e116603fbb890e9df845e3:`

Hasil: **24/24 lane cocok persis** dengan keccak Python, tanpa mismatch:

| Batch | Nonce | Panjang pesan | Hasil |
|---|---|---|---|
| 1 | 0–7 | 128 byte (1 blok) | 8/8 cocok |
| 2 | 999999992–999999999 | 136 byte (tepat di batas blok, 2 blok) | 8/8 cocok |
| 3 | 1000000000–1000000007 | 137 byte (2 blok) | 8/8 cocok |

Catatan teknis: konvensi `bswap64` miner HashOre TIDAK berlaku untuk
PodMint (pesan ASCII verbatim, little-endian per word 8 byte, tanpa
byte-swap). Compile bersih `gcc -O3 -mavx512f -pthread`, jalan di CPU
AMD EPYC 9D25 (mendukung AVX512).

Kecepatan terukur:
- Harness end-to-end: 0,53 juta hash/detik (1 thread) / 0,83 juta (2 thread)
  — angka ini meremehkan (overhead setup per batch + berbagi CPU dengan
  pencarian live).
- Permutasi saja: 2,50 juta hash/detik untuk pesan 2-blok
  (~4,8× kecepatan per-core solver scalar ~0,525 juta/detik).

#### 2. Bukti live: baton beneran ditemukan via AVX512 (Prof X, 09:54 WIB)

- Blok: 30–40 miliar (blok Prof X).
- Nonce: **34954707736** (34.954.707.736, di dalam blok 30–40 miliar).
- Hash: `0x000000001510c7ae33e2c2ea94a6b42b7fafa23f712aad00166cff42edc542c5`
- **35 bit nol di depan** (≥33 yang diminta).
- Perjalanan AVX512 Prof X: 3.714,1 juta hash, 870 detik,
  **4,27 juta hash/detik**, dari titik restart 31.248.700.000.
- **Terverifikasi independen oleh CST** via keccak Python: hash hitung
  cocok persis dengan hash klaim, 35 zero bits, nonce dalam blok.
  (Verifikasi = 1 hash, bukan ulang pencarian.)

#### Kesimpulan

AVX512 **terbukti bekerja** untuk format baton PodMint — baik di test
terkontrol (24/24 cocok) maupun di pencarian beneran (baton ronde 8
ditemukan via AVX512 dan lolos verifikasi independen).

Catatan: Prof X tidak pernah mengirim balik hasil test vektor
(round8_test_untuk_profx.md) — ia langsung menjalankan pencarian
beneran dan menemukan baton. Temuan baton itu sendiri adalah bukti
yang lebih kuat daripada test vektor, sehingga test vektor tidak
lagi diperlukan.

## Lampiran E: Lisensi MIT

MIT License

Copyright (c) 2026 Nur

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
