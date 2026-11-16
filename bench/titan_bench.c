/* CPU benchmark suite for the TITAN-NT Wine bundle. One source builds three
 * PE targets: x86_64 runs through FEX, i686 through WoW64, aarch64 natively.
 * stdout is JSONL only; progress goes to stderr. */
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lz4.h"
#include "zstd.h"

#if defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
#define BENCH_ARCH "aarch64"
#elif defined(_M_X64) || defined(__x86_64__)
#define BENCH_ARCH "x86_64"
#elif defined(_M_IX86) || defined(__i386__)
#define BENCH_ARCH "i686"
#else
#define BENCH_ARCH "unknown"
#endif

#define MAX_REPEATS 15
#define ITER_CAP 4000000000ULL
#define MT_MAX 8
#define BRANCH_N (1u << 20)
#define CORPUS_N (4u << 20)
#define CHASE_MB 64

static volatile unsigned long long sink;
static int failures;
static int measurements;
static int repeats = 5;
static double target_ns = 0.35e9;
static int run_error;

static char only_buf[256];
static char *only_tokens[16];
static int only_count;

struct result {
    int ran;
    int ok;
    int n;
    double med, lo, hi, cpu;
    unsigned long long iters, ck;
};

static double qpc_ns_per_tick;

static unsigned long long now_ns(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned long long)((double)c.QuadPart * qpc_ns_per_tick);
}

static double cpu_seconds(void)
{
    FILETIME ct, et, kt, ut;
    ULARGE_INTEGER k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) return 0.0;
    k.LowPart = kt.dwLowDateTime;
    k.HighPart = kt.dwHighDateTime;
    u.LowPart = ut.dwLowDateTime;
    u.HighPart = ut.dwHighDateTime;
    return (double)(k.QuadPart + u.QuadPart) * 1e-7;
}

static void mem_info(unsigned long long *peak_kb, unsigned long long *private_kb)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
        *peak_kb = (unsigned long long)pmc.PeakWorkingSetSize / 1024;
        *private_kb = (unsigned long long)pmc.PrivateUsage / 1024;
    } else {
        *peak_kb = 0;
        *private_kb = 0;
    }
}

static unsigned long long f64_bits(double v)
{
    unsigned long long u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static unsigned long long f32_bits(float v)
{
    unsigned int u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

/* ---------------------------------------------------------------- integer */

static NOINLINE unsigned long long int_dep_run(unsigned long long iters)
{
    uint32_t a = 0x9e3779b9u;
    unsigned long long i;
    for (i = 0; i < iters; i++) {
        a += 0x7f4a7c15u;
        a ^= a >> 13;
        a = (a << 7) | (a >> 25);
        a *= 0x85ebca6bu;
    }
    sink ^= a;
    return a;
}

/* The empty asm keeps the four chains scalar; otherwise SLP vectorises them
 * and the workload stops measuring integer issue width. */
static NOINLINE unsigned long long int_indep_body(unsigned long long iters)
{
    uint32_t a0 = 0x243f6a88u, a1 = 0x85a308d3u;
    uint32_t a2 = 0x13198a2eu, a3 = 0x03707344u;
    unsigned long long i;
    for (i = 0; i < iters; i++) {
        a0 += 0x7f4a7c15u; a0 ^= a0 >> 13; a0 = (a0 << 7) | (a0 >> 25); a0 *= 0x85ebca6bu;
        a1 += 0x9e3779b9u; a1 ^= a1 >> 17; a1 = (a1 << 11) | (a1 >> 21); a1 *= 0xc2b2ae35u;
        a2 += 0x6a09e667u; a2 ^= a2 >> 7;  a2 = (a2 << 19) | (a2 >> 13); a2 *= 0x27d4eb2fu;
        a3 += 0xbb67ae85u; a3 ^= a3 >> 23; a3 = (a3 << 5) | (a3 >> 27);  a3 *= 0x165667b1u;
        __asm__ __volatile__("" : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3));
    }
    return ((unsigned long long)a0 << 32) ^ ((unsigned long long)a1 << 16)
         ^ (unsigned long long)a2 ^ ((unsigned long long)a3 << 48);
}

static NOINLINE unsigned long long int_indep_run(unsigned long long iters)
{
    unsigned long long r = int_indep_body(iters);
    sink ^= r;
    return r;
}

/* --------------------------------------------------------------- floating */

/* -ffp-contract=off keeps this a separate multiply and add on every target,
 * so the op count and the dependency latency match across architectures. */
static NOINLINE unsigned long long fp_double_fma_run(unsigned long long iters)
{
    double a = 1.0;
    const double m = 0.9999999, c = 1e-7;
    unsigned long long i;
    for (i = 0; i < iters; i++) a = a * m + c;
    sink ^= f64_bits(a);
    return f64_bits(a);
}

static NOINLINE unsigned long long fp_double_sqrt_run(unsigned long long iters)
{
    double a = 1.5;
    unsigned long long i;
    for (i = 0; i < iters; i++) a = __builtin_sqrt(a + 1.0);
    sink ^= f64_bits(a);
    return f64_bits(a);
}

static NOINLINE unsigned long long fp_float_fma_run(unsigned long long iters)
{
    float a = 1.0f;
    const float m = 0.999999f, c = 1e-6f;
    unsigned long long i;
    for (i = 0; i < iters; i++) a = a * m + c;
    sink ^= f32_bits(a);
    return f32_bits(a);
}

static NOINLINE unsigned long long fp_float_sqrt_run(unsigned long long iters)
{
    float a = 1.5f;
    unsigned long long i;
    for (i = 0; i < iters; i++) a = __builtin_sqrtf(a + 1.0f);
    sink ^= f32_bits(a);
    return f32_bits(a);
}

/* ---------------------------------------------------------------- branch */

static unsigned char *branch_data;

/* The asm barrier in both arms is a side effect, so the compiler cannot
 * if-convert the arm into a cmov/csel and erase the mispredict cost. */
NOINLINE unsigned long long branch_dependent_run(unsigned long long passes)
{
    uint32_t sum = 0;
    unsigned long long p;
    unsigned i;
    for (p = 0; p < passes; p++) {
        for (i = 0; i < BRANCH_N; i++) {
            unsigned b = branch_data[i];
            if (b & 0x80u) {
                sum += b;
                __asm__ __volatile__("" : "+r"(sum));
            } else {
                sum -= b;
                __asm__ __volatile__("" : "+r"(sum));
            }
        }
    }
    sink ^= sum;
    return sum;
}

NOINLINE unsigned long long branch_branchless_run(unsigned long long passes)
{
    uint32_t sum = 0;
    unsigned long long p;
    unsigned i;
    for (p = 0; p < passes; p++) {
        for (i = 0; i < BRANCH_N; i++) {
            unsigned b = branch_data[i];
            uint32_t mask = (b >> 7) - 1u;
            sum += (b ^ mask) - mask;
        }
    }
    sink ^= sum;
    return sum;
}

/* ---------------------------------------------------------------- memory */

static unsigned char *chase_buf;
static size_t chase_n;
static size_t chase_start;

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint32_t rng32(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545f4914f6cdd1dULL) >> 32);
}

/* A random permutation defeats the hardware stride prefetcher, so the chain
 * really pays one cache miss per access. */
static int chase_init(void)
{
    size_t i;
    uint32_t *perm;
    size_t n = (size_t)CHASE_MB * 1024 * 1024 / 64;

    chase_buf = malloc(n * 64);
    perm = malloc(n * sizeof(*perm));
    if (!chase_buf || !perm) {
        free(chase_buf);
        free(perm);
        chase_buf = NULL;
        return 0;
    }
    /* Per-workload seed so the permutation does not depend on which earlier
     * workloads consumed the shared generator. */
    rng_state = 0xfedcba9876543210ULL;
    for (i = 0; i < n; i++) perm[i] = (uint32_t)i;
    for (i = n - 1; i > 0; i--) {
        size_t j = (size_t)(rng32() % (uint32_t)(i + 1));
        uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    for (i = 0; i < n; i++) {
        size_t cur = (size_t)perm[i] * 64;
        size_t next = (size_t)perm[(i + 1) % n] * 64;
        *(size_t *)(chase_buf + cur) = next;
    }
    chase_n = n;
    chase_start = (size_t)perm[0] * 64;
    free(perm);
    return 1;
}

static NOINLINE unsigned long long chase_run(unsigned long long cycles)
{
    size_t idx = chase_start;
    unsigned long long sum = 0;
    unsigned long long c;
    size_t i;
    for (c = 0; c < cycles; c++) {
        for (i = 0; i < chase_n; i++) {
            idx = *(size_t *)(chase_buf + idx);
            sum += (unsigned long long)idx;
        }
    }
    sink ^= sum;
    return sum;
}

static size_t stream_size;
static unsigned char *stream_src;
static unsigned char *stream_dst;
static void *(*volatile stream_memcpy)(void *, const void *, size_t) = memcpy;

static NOINLINE unsigned long long stream_run(unsigned long long iters)
{
    unsigned long long i;
    unsigned long long r;
    for (i = 0; i < iters; i++)
        stream_memcpy(stream_dst, stream_src, stream_size);
    r = (unsigned long long)stream_dst[0]
      ^ ((unsigned long long)stream_dst[stream_size - 1] << 8);
    sink ^= r;
    return r;
}

static int stream_init(void)
{
    size_t max = (size_t)32 * 1024 * 1024;
    stream_src = malloc(max);
    stream_dst = malloc(max);
    if (!stream_src || !stream_dst) return 0;
    memset(stream_src, 0x5a, max);
    memset(stream_dst, 0xa5, max);
    return 1;
}

/* ----------------------------------------------------------- compression */

static unsigned char *corpus;
static unsigned char *lz4_comp;
static unsigned char *zstd_comp;
static unsigned char *dec_buf;
static int lz4_cap, zstd_cap;
static int lz4_csize, zstd_csize;

static const char *const words[] = {
    "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
    "kernel", "memory", "thread", "process", "buffer", "window", "device",
    "android", "translator", "cache", "branch", "vector", "clock", "power",
    "thermal", "cluster", "scheduler", "latency", "bandwidth", "compress",
    "decompress", "entropy", "dictionary", "stream"
};

static void corpus_fill(void)
{
    size_t block;
    rng_state = 0x0f1e2d3c4b5a6978ULL;
    for (block = 0; block < CORPUS_N / 65536; block++) {
        unsigned char *p = corpus + block * 65536;
        if (block & 1) {
            size_t i;
            for (i = 0; i < 65536; i++) p[i] = (unsigned char)rng32();
        } else {
            size_t off = 0;
            unsigned col = 0;
            while (off < 65536) {
                const char *w = words[rng32() % (sizeof(words) / sizeof(words[0]))];
                size_t l = strlen(w);
                if (off + l + 1 > 65536) break;
                memcpy(p + off, w, l);
                off += l;
                p[off++] = (col % 12 == 11) ? '\n' : ' ';
                col++;
            }
            while (off < 65536) p[off++] = '\n';
        }
    }
}

static int compression_init(void)
{
    lz4_cap = LZ4_compressBound((int)CORPUS_N);
    zstd_cap = (int)ZSTD_compressBound(CORPUS_N);
    corpus = malloc(CORPUS_N);
    lz4_comp = malloc((size_t)lz4_cap);
    zstd_comp = malloc((size_t)zstd_cap);
    dec_buf = malloc(CORPUS_N);
    if (!corpus || !lz4_comp || !zstd_comp || !dec_buf) return 0;

    corpus_fill();

    lz4_csize = LZ4_compress_default((const char *)corpus, (char *)lz4_comp,
                                     (int)CORPUS_N, lz4_cap);
    if (lz4_csize <= 0) return 0;
    if (LZ4_decompress_safe((const char *)lz4_comp, (char *)dec_buf,
                            lz4_csize, (int)CORPUS_N) != (int)CORPUS_N) return 0;
    if (memcmp(dec_buf, corpus, CORPUS_N) != 0) return 0;

    zstd_csize = (int)ZSTD_compress(zstd_comp, (size_t)zstd_cap,
                                    corpus, CORPUS_N, 3);
    if (ZSTD_isError((size_t)zstd_csize)) return 0;
    if (ZSTD_decompress(dec_buf, CORPUS_N, zstd_comp,
                        (size_t)zstd_csize) != CORPUS_N) return 0;
    if (memcmp(dec_buf, corpus, CORPUS_N) != 0) return 0;
    return 1;
}

static NOINLINE unsigned long long lz4_compress_run(unsigned long long iters)
{
    unsigned long long acc = 0;
    unsigned long long i;
    for (i = 0; i < iters; i++) {
        int n = LZ4_compress_default((const char *)corpus, (char *)lz4_comp,
                                     (int)CORPUS_N, lz4_cap);
        if (n <= 0) { run_error = 1; return 0; }
        lz4_csize = n;
        acc += (unsigned)n;
    }
    sink ^= acc;
    return acc;
}

static NOINLINE unsigned long long lz4_decompress_run(unsigned long long iters)
{
    unsigned long long i;
    for (i = 0; i < iters; i++) {
        int n = LZ4_decompress_safe((const char *)lz4_comp, (char *)dec_buf,
                                    lz4_csize, (int)CORPUS_N);
        if (n != (int)CORPUS_N) { run_error = 1; return 0; }
    }
    return ((unsigned long long)dec_buf[0] << 8) | dec_buf[CORPUS_N - 1];
}

static NOINLINE unsigned long long zstd_compress_run(unsigned long long iters)
{
    unsigned long long acc = 0;
    unsigned long long i;
    for (i = 0; i < iters; i++) {
        size_t n = ZSTD_compress(zstd_comp, (size_t)zstd_cap, corpus,
                                 CORPUS_N, 3);
        if (ZSTD_isError(n)) { run_error = 1; return 0; }
        zstd_csize = (int)n;
        acc += (unsigned long long)n;
    }
    sink ^= acc;
    return acc;
}

static NOINLINE unsigned long long zstd_decompress_run(unsigned long long iters)
{
    unsigned long long i;
    for (i = 0; i < iters; i++) {
        size_t n = ZSTD_decompress(dec_buf, CORPUS_N, zstd_comp,
                                   (size_t)zstd_csize);
        if (ZSTD_isError(n) || n != CORPUS_N) { run_error = 1; return 0; }
    }
    return ((unsigned long long)dec_buf[0] << 8) | dec_buf[CORPUS_N - 1];
}

/* ------------------------------------------------------------ measuring */

typedef unsigned long long (*run_fn)(unsigned long long iters);

static void fail(const char *name, const char *why)
{
    printf("{\"bench\":\"cpu\",\"name\":\"%s\",\"error\":\"%s\"}\n", name, why);
    failures++;
}

static int selected(const char *group, const char *name)
{
    int i;
    if (!only_count) return 1;
    for (i = 0; i < only_count; i++) {
        if (!strcmp(only_tokens[i], group)) return 1;
        if (name && !strcmp(only_tokens[i], name)) return 1;
    }
    return 0;
}

static unsigned long long calibrate(run_fn fn, double target)
{
    unsigned long long iters = 1;
    int i;
    for (i = 0; i < 20; i++) {
        unsigned long long t0, t1;
        double ns, want;
        t0 = now_ns();
        fn(iters);
        t1 = now_ns();
        if (run_error) return iters;
        ns = (double)(t1 - t0);
        if (ns >= target) return iters;
        if (ns <= 0.0) {
            if (iters > ITER_CAP / 16) return ITER_CAP;
            iters *= 16;
        } else {
            want = (double)iters * (target * 1.25 / ns);
            if (want < (double)iters * 1.05) want = (double)iters * 1.05;
            if (want > (double)ITER_CAP) want = (double)ITER_CAP;
            iters = (unsigned long long)want;
        }
        if (iters >= ITER_CAP) return ITER_CAP;
    }
    return iters;
}

static void sort_samples(double *ns, double *cpu, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        double kn = ns[i], kc = cpu[i];
        j = i - 1;
        while (j >= 0 && ns[j] > kn) {
            ns[j + 1] = ns[j];
            cpu[j + 1] = cpu[j];
            j--;
        }
        ns[j + 1] = kn;
        cpu[j + 1] = kc;
    }
}

static struct result measure(const char *group, const char *name,
                             run_fn fn, double target)
{
    struct result r;
    double ns[MAX_REPEATS], cp[MAX_REPEATS];
    unsigned long long iters, ck = 0;
    int k, n = 0;

    memset(&r, 0, sizeof(r));
    if (!selected(group, name)) return r;
    r.ran = 1;

    iters = calibrate(fn, target);
    if (run_error) {
        fail(name, "run");
        run_error = 0;
        return r;
    }
    fn(iters);
    if (run_error) {
        fail(name, "run");
        run_error = 0;
        return r;
    }

    for (k = 0; k < repeats; k++) {
        double c0, c1;
        unsigned long long t0, t1;
        c0 = cpu_seconds();
        t0 = now_ns();
        ck = fn(iters);
        t1 = now_ns();
        c1 = cpu_seconds();
        if (run_error) break;
        ns[n] = (double)(t1 - t0);
        cp[n] = ns[n] > 0.0 ? (c1 - c0) / (ns[n] * 1e-9) : 0.0;
        n++;
    }
    if (run_error || n == 0) {
        fail(name, "run");
        run_error = 0;
        return r;
    }
    sort_samples(ns, cp, n);
    r.n = n;
    r.med = (n & 1) ? ns[n / 2] : (ns[n / 2 - 1] + ns[n / 2]) / 2.0;
    r.lo = ns[0];
    r.hi = ns[n - 1];
    r.cpu = cp[n / 2];
    r.iters = iters;
    r.ck = ck;
    r.ok = 1;
    return r;
}

static void emit(const char *name, const struct result *r, double ops_per_iter,
                 const char *extra)
{
    unsigned long long peak, priv;
    double ops = ops_per_iter * (double)r->iters / (r->med * 1e-9);
    mem_info(&peak, &priv);
    printf("{\"bench\":\"cpu\",\"name\":\"%s\",\"ns\":%llu,\"ops_per_s\":%.6g,"
           "\"cpu\":%.3f,\"peak_rss_kb\":%llu,\"private_kb\":%llu,"
           "\"min_ns\":%llu,\"max_ns\":%llu,\"iters\":%llu,\"repeats\":%d,"
           "\"checksum\":%llu%s}\n",
           name, (unsigned long long)(r->med + 0.5), ops, r->cpu, peak, priv,
           (unsigned long long)(r->lo + 0.5),
           (unsigned long long)(r->hi + 0.5),
           r->iters, r->n, r->ck, extra ? extra : "");
    measurements++;
}

/* --------------------------------------------------------- multithreading */

struct mt_arg {
    HANDLE start;
    unsigned long long iters;
    unsigned long long ck;
};

static DWORD WINAPI mt_worker(void *p)
{
    struct mt_arg *a = p;
    WaitForSingleObject(a->start, INFINITE);
    a->ck = int_indep_body(a->iters);
    return 0;
}

/* Thread creation stays outside the timed window; the join is inside it so
 * a thread that starts late still counts against wall time. */
static int mt_once(int threads, unsigned long long iters,
                   double *out_ns, double *out_cpu, unsigned long long *out_ck)
{
    HANDLE start, th[MT_MAX];
    struct mt_arg args[MT_MAX];
    int created = 0, i;
    DWORD w;
    double c0, c1;
    unsigned long long t0, t1, ck = 0;

    start = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!start) return 0;
    for (i = 0; i < threads; i++) {
        args[i].start = start;
        args[i].iters = iters;
        args[i].ck = 0;
        th[i] = CreateThread(NULL, 0, mt_worker, &args[i], 0, NULL);
        if (!th[i]) break;
        created++;
    }
    if (created != threads) {
        SetEvent(start);
        for (i = 0; i < created; i++) {
            WaitForSingleObject(th[i], 60000);
            CloseHandle(th[i]);
        }
        CloseHandle(start);
        return 0;
    }

    c0 = cpu_seconds();
    t0 = now_ns();
    SetEvent(start);
    w = WaitForMultipleObjects((DWORD)threads, th, TRUE, 300000);
    t1 = now_ns();
    c1 = cpu_seconds();

    for (i = 0; i < threads; i++) {
        WaitForSingleObject(th[i], 1000);
        CloseHandle(th[i]);
        ck ^= args[i].ck;
    }
    CloseHandle(start);
    if (w != WAIT_OBJECT_0) return 0;

    *out_ns = (double)(t1 - t0);
    *out_cpu = *out_ns > 0.0 ? (c1 - c0) / (*out_ns * 1e-9) : 0.0;
    *out_ck = ck;
    return 1;
}

static struct result measure_mt(const char *name, int threads,
                                unsigned long long iters)
{
    struct result r;
    double ns[MAX_REPEATS], cp[MAX_REPEATS];
    unsigned long long ck = 0;
    int k, n = 0;

    memset(&r, 0, sizeof(r));
    if (!selected("int_mt", name)) return r;
    r.ran = 1;

    if (!mt_once(threads, iters, &ns[0], &cp[0], &ck)) {
        fail(name, "thread");
        return r;
    }
    for (k = 0; k < repeats; k++) {
        if (!mt_once(threads, iters, &ns[n], &cp[n], &ck)) {
            fail(name, "thread");
            return r;
        }
        n++;
    }
    sort_samples(ns, cp, n);
    r.n = n;
    r.med = (n & 1) ? ns[n / 2] : (ns[n / 2 - 1] + ns[n / 2]) / 2.0;
    r.lo = ns[0];
    r.hi = ns[n - 1];
    r.cpu = cp[n / 2];
    r.iters = iters;
    r.ck = ck;
    r.ok = 1;
    return r;
}

/* ------------------------------------------------------------------ main */

static void list_workloads(void)
{
    printf("int_alu_dependent\nint_alu_independent\n"
           "fp_double_fma\nfp_double_sqrt\nfp_float_fma\nfp_float_sqrt\n"
           "branch_dependent\nbranch_branchless\nmem_chase\n"
           "mem_stream_4k\nmem_stream_64k\nmem_stream_1m\nmem_stream_8m\n"
           "mem_stream_32m\nint_mt_1t\nint_mt_2t\nint_mt_4t\nint_mt_8t\n"
           "lz4_compress\nlz4_decompress\nzstd_compress\nzstd_decompress\n");
}

static void run_all(void)
{
    struct result r;
    double t = target_ns;
    char extra[256];

    r = measure("int_alu", "int_alu_dependent", int_dep_run, t);
    if (r.ok) emit("int_alu_dependent", &r, 4.0, NULL);

    r = measure("int_alu", "int_alu_independent", int_indep_run, t);
    if (r.ok) emit("int_alu_independent", &r, 16.0, NULL);

    r = measure("fp", "fp_double_fma", fp_double_fma_run, t);
    if (r.ok) emit("fp_double_fma", &r, 2.0, NULL);

    r = measure("fp", "fp_double_sqrt", fp_double_sqrt_run, t);
    if (r.ok) emit("fp_double_sqrt", &r, 2.0, NULL);

    r = measure("fp", "fp_float_fma", fp_float_fma_run, t);
    if (r.ok) emit("fp_float_fma", &r, 2.0, NULL);

    r = measure("fp", "fp_float_sqrt", fp_float_sqrt_run, t);
    if (r.ok) emit("fp_float_sqrt", &r, 2.0, NULL);

    if (selected("branch", "branch_dependent") ||
        selected("branch", "branch_branchless")) {
        branch_data = malloc(BRANCH_N);
        if (!branch_data) {
            if (selected("branch", "branch_dependent")) fail("branch_dependent", "alloc");
            if (selected("branch", "branch_branchless")) fail("branch_branchless", "alloc");
        } else {
            unsigned i;
            rng_state = 0x123456789abcdef0ULL;
            for (i = 0; i < BRANCH_N; i++) branch_data[i] = (unsigned char)rng32();
        }
    }
    if (branch_data) {
        struct result dep, les;
        dep = measure("branch", "branch_dependent", branch_dependent_run, t);
        les = measure("branch", "branch_branchless", branch_branchless_run, t);
        /* Both variants must compute the same sum; a mismatch means the
         * branchless rewrite is not measuring the same work. */
        if (dep.ok && les.ok && dep.ck != les.ck) {
            fail("branch_branchless", "mismatch");
        } else {
            if (dep.ok) {
                double bytes = (double)dep.iters * BRANCH_N / (dep.med * 1e-9);
                snprintf(extra, sizeof(extra), ",\"mb_per_s\":%.6g", bytes / 1e6);
                emit("branch_dependent", &dep, (double)BRANCH_N, extra);
            }
            if (les.ok) {
                double bytes = (double)les.iters * BRANCH_N / (les.med * 1e-9);
                snprintf(extra, sizeof(extra), ",\"mb_per_s\":%.6g", bytes / 1e6);
                emit("branch_branchless", &les, (double)BRANCH_N, extra);
            }
        }
        free(branch_data);
        branch_data = NULL;
    }

    if (selected("mem_chase", "mem_chase")) {
        if (!chase_init()) {
            fail("mem_chase", "alloc");
        } else {
            r = measure("mem_chase", "mem_chase", chase_run, t);
            if (r.ok) {
                double ns_per = r.med / ((double)r.iters * (double)chase_n);
                snprintf(extra, sizeof(extra),
                         ",\"ns_per_access\":%.4f,\"buffer_mb\":%d",
                         ns_per, CHASE_MB);
                emit("mem_chase", &r, (double)chase_n, extra);
            }
            free(chase_buf);
            chase_buf = NULL;
        }
    }

    {
        static const struct { const char *name; size_t size; } sizes[] = {
            { "mem_stream_4k", 4u * 1024 },
            { "mem_stream_64k", 64u * 1024 },
            { "mem_stream_1m", 1u << 20 },
            { "mem_stream_8m", 8u << 20 },
            { "mem_stream_32m", 32u << 20 }
        };
        size_t i;
        int any = 0;
        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            any |= selected("mem_stream", sizes[i].name);
        if (any) {
            if (!stream_init()) {
                for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
                    if (selected("mem_stream", sizes[i].name))
                        fail(sizes[i].name, "alloc");
            } else {
                for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
                    double bytes;
                    stream_size = sizes[i].size;
                    r = measure("mem_stream", sizes[i].name, stream_run, t);
                    if (!r.ok) continue;
                    bytes = (double)r.iters * (double)sizes[i].size
                          / (r.med * 1e-9);
                    snprintf(extra, sizeof(extra),
                             ",\"mb_per_s\":%.6g,\"size_bytes\":%llu,"
                             "\"ns_per_copy\":%llu",
                             bytes / 1e6, (unsigned long long)sizes[i].size,
                             (unsigned long long)(r.med + 0.5));
                    emit(sizes[i].name, &r, (double)sizes[i].size, extra);
                }
                free(stream_src);
                free(stream_dst);
                stream_src = stream_dst = NULL;
            }
        }
    }

    {
        static const int counts[] = { 1, 2, 4, 8 };
        struct result res[4];
        double ops[4] = { 0, 0, 0, 0 };
        char names[4][16];
        char mt_extra[256];
        int i, any = 0;
        for (i = 0; i < 4; i++) {
            snprintf(names[i], sizeof(names[i]), "int_mt_%dt", counts[i]);
            any |= selected("int_mt", names[i]);
        }
        if (any) {
            unsigned long long mt_iters = calibrate(int_indep_run, t);
            if (run_error) {
                run_error = 0;
                for (i = 0; i < 4; i++)
                    if (selected("int_mt", names[i])) fail(names[i], "run");
            } else {
                for (i = 0; i < 4; i++) {
                    res[i] = measure_mt(names[i], counts[i], mt_iters);
                    if (res[i].ok)
                        ops[i] = 16.0 * (double)counts[i]
                               * (double)res[i].iters / (res[i].med * 1e-9);
                }
                for (i = 0; i < 4; i++) {
                    if (!res[i].ok) continue;
                    if (ops[0] > 0.0)
                        snprintf(mt_extra, sizeof(mt_extra),
                                 ",\"threads\":%d,\"scaling\":%.4f,"
                                 "\"per_thread_ops_per_s\":%.6g",
                                 counts[i], ops[i] / ops[0],
                                 ops[i] / counts[i]);
                    else
                        snprintf(mt_extra, sizeof(mt_extra),
                                 ",\"threads\":%d,\"per_thread_ops_per_s\":%.6g",
                                 counts[i], ops[i] / counts[i]);
                    emit(names[i], &res[i], 16.0 * (double)counts[i], mt_extra);
                }
            }
        }
    }

    if (selected("lz4", "lz4_compress") || selected("lz4", "lz4_decompress") ||
        selected("zstd", "zstd_compress") || selected("zstd", "zstd_decompress")) {
        if (!compression_init()) {
            if (selected("lz4", "lz4_compress")) fail("lz4_compress", "init");
            if (selected("lz4", "lz4_decompress")) fail("lz4_decompress", "init");
            if (selected("zstd", "zstd_compress")) fail("zstd_compress", "init");
            if (selected("zstd", "zstd_decompress")) fail("zstd_decompress", "init");
        } else {
            r = measure("lz4", "lz4_compress", lz4_compress_run, t);
            if (r.ok) {
                snprintf(extra, sizeof(extra),
                         ",\"mb_per_s\":%.6g,\"ratio\":%.4f,"
                         "\"compressed_bytes\":%d,\"original_bytes\":%u",
                         (double)CORPUS_N * (double)r.iters / (r.med * 1e-9) / 1e6,
                         (double)lz4_csize / (double)CORPUS_N,
                         lz4_csize, (unsigned)CORPUS_N);
                emit("lz4_compress", &r, (double)CORPUS_N, extra);
            }
            r = measure("lz4", "lz4_decompress", lz4_decompress_run, t);
            if (r.ok) {
                snprintf(extra, sizeof(extra),
                         ",\"mb_per_s\":%.6g,\"ratio\":%.4f,"
                         "\"compressed_bytes\":%d,\"original_bytes\":%u",
                         (double)CORPUS_N * (double)r.iters / (r.med * 1e-9) / 1e6,
                         (double)lz4_csize / (double)CORPUS_N,
                         lz4_csize, (unsigned)CORPUS_N);
                emit("lz4_decompress", &r, (double)CORPUS_N, extra);
            }
            r = measure("zstd", "zstd_compress", zstd_compress_run, t);
            if (r.ok) {
                snprintf(extra, sizeof(extra),
                         ",\"mb_per_s\":%.6g,\"ratio\":%.4f,\"level\":3,"
                         "\"compressed_bytes\":%d,\"original_bytes\":%u",
                         (double)CORPUS_N * (double)r.iters / (r.med * 1e-9) / 1e6,
                         (double)zstd_csize / (double)CORPUS_N,
                         zstd_csize, (unsigned)CORPUS_N);
                emit("zstd_compress", &r, (double)CORPUS_N, extra);
            }
            r = measure("zstd", "zstd_decompress", zstd_decompress_run, t);
            if (r.ok) {
                snprintf(extra, sizeof(extra),
                         ",\"mb_per_s\":%.6g,\"ratio\":%.4f,\"level\":3,"
                         "\"compressed_bytes\":%d,\"original_bytes\":%u",
                         (double)CORPUS_N * (double)r.iters / (r.med * 1e-9) / 1e6,
                         (double)zstd_csize / (double)CORPUS_N,
                         zstd_csize, (unsigned)CORPUS_N);
                emit("zstd_decompress", &r, (double)CORPUS_N, extra);
            }
        }
    }
}

int main(int argc, char **argv)
{
    char binary[MAX_PATH];
    char stamp[64];
    LARGE_INTEGER f;
    time_t now;
    struct tm *g;
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    QueryPerformanceFrequency(&f);
    qpc_ns_per_tick = 1e9 / (double)f.QuadPart;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all")) {
            continue;
        } else if (!strcmp(argv[i], "--list")) {
            list_workloads();
            return 0;
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
            target_ns = atof(argv[++i]) * 1e9;
        } else if (!strcmp(argv[i], "--repeats") && i + 1 < argc) {
            repeats = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
            snprintf(only_buf, sizeof(only_buf), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr,
                    "usage: %s [--all] [--only name[,name]] "
                    "[--seconds s] [--repeats n] [--list]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (target_ns < 0.02e9) target_ns = 0.02e9;
    if (target_ns > 5.0e9) target_ns = 5.0e9;
    if (repeats < 1) repeats = 1;
    if (repeats > MAX_REPEATS) repeats = MAX_REPEATS;

    if (only_buf[0]) {
        char *p = only_buf;
        while (*p && only_count < 16) {
            char *comma;
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            only_tokens[only_count++] = p;
            comma = strchr(p, ',');
            if (!comma) break;
            *comma = '\0';
            p = comma + 1;
        }
    }

    GetModuleFileNameA(NULL, binary, sizeof(binary));
    {
        const char *base = strrchr(binary, '\\');
        base = base ? base + 1 : binary;
        printf("{\"bench\":\"start\",\"binary\":\"%s\",\"arch\":\"%s\","
               "\"repeats\":%d,\"target_s\":%.3f}\n",
               base, BENCH_ARCH, repeats, target_ns / 1e9);
    }

    run_all();

    now = time(NULL);
    g = gmtime(&now);
    if (g) strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", g);
    else snprintf(stamp, sizeof(stamp), "unknown");

    {
        const char *base = strrchr(binary, '\\');
        base = base ? base + 1 : binary;
        printf("{\"bench\":\"summary\",\"binary\":\"%s\",\"arch\":\"%s\","
               "\"workloads\":%d,\"failures\":%d,\"timestamp\":\"%s\","
               "\"checksum\":%llu}\n",
               base, BENCH_ARCH, measurements, failures, stamp,
               (unsigned long long)sink);
    }
    return failures ? 1 : 0;
}
