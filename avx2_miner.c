// AVX512 SSr — Fast parallel Keccak miner for nonce search.
// This file is part of the AVX512 SSr product.
//
// avx2_miner.c — AVX2 4-way Keccak-256 nonce miner.
// Production miner (search loop + constant-block precompute + multi-thread
// + progress + checkpoint), mirroring the CLI of avx512_miner.
//
// Message per lane = prefix (ASCII) + decimal nonce (ASCII, no spaces).
// NO byte-swapping anywhere: ASCII bytes go in verbatim, little-endian
// per 8-byte word, exactly as Keccak reference implementations do.
// No other byte-swap convention applies here.
// Padding is Keccak pad10*1 (0x01 ... 0x80), rate 136 bytes.
//
// Purely local computation. No network, wallet, or submission of results
// to any party. Never use this miner to act on a result that has not
// been independently verified.
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

// Hash 4 messages (prefix + nonces[0..3]) -> out[4][32].
// base_st = state after absorbing the constant blocks (zero state when
// there are none); start_block = first block index NOT in base_st.
// Lanes finishing in an earlier block have their digest saved right after
// that block's permutation, so batches whose lanes have different block
// counts (digit-length boundaries) are still correct.
static void keccak_batch4(const char *prefix, const uint64_t nonces[4],
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
        keccak_batch4(prefix, batch, out, base, nconst);
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
