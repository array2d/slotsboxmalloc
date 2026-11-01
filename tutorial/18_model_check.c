#define _POSIX_C_SOURCE 200809L
/* Randomized model check: no overlap, exact capacity, no false OOM. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stdatomic.h>
#include <time.h>
#include <slotsboxmalloc/slotsboxobj.h>

#define META_SIZE (16 * 1024 * 1024)
#define DATA_SIZE (8ULL * 64 * 64 * 64 * 64) /* 128MB */
#define UNITS (DATA_SIZE / 8)
#define FAIL64 ((uint64_t)-1)
#define LIVE 1024

static const size_t SZS[] = {1, 5, 8, 9, 16, 64, 100, 512, 1000, 4096, 8192, 16384, 65536, 200000, 1000000};
#define NSZS (sizeof(SZS) / sizeof(SZS[0]))

static uint8_t *g_meta, *g_data;
static _Atomic uint8_t *g_claim;
static _Atomic long g_err, g_phaseb_fail, g_alloc_ok;

static uint64_t rng(uint64_t *s) {
    uint64_t x = *s; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *s = x;
    return x * 2685821657736338717ULL;
}

/* canonical capacity: 8 * 64^(l-1) * m with m in [1,63] */
static uint64_t expect_cap(size_t sz) {
    uint64_t n = (sz + 7) / 8;
    if (n <= 63) return n * 8;
    uint64_t base = 1;
    while (63 * base < n) base *= 64;
    return ((n + base - 1) / base) * base * 8;
}

typedef struct { uint64_t off, cap, tag; } ent_t;

static void verify_and_free(ent_t *e, uint8_t owner) {
    if (sbo_allocated_size(g_meta, e->off) != e->cap) g_err++;
    uint64_t u0 = e->off / 8, un = e->cap / 8;
    for (uint64_t u = u0; u < u0 + un; u++)
        if (atomic_load_explicit(&g_claim[u], memory_order_relaxed) != owner) { g_err++; return; }
    uint64_t v0, v1;
    memcpy(&v0, g_data + e->off, 8);
    memcpy(&v1, g_data + e->off + e->cap - 8, 8);
    if (v0 != e->tag || v1 != e->tag) g_err++;
    for (uint64_t u = u0; u < u0 + un; u++)
        atomic_store_explicit(&g_claim[u], 0, memory_order_relaxed);
    sbo_free(g_meta, e->off);
}

typedef struct { int tid, iters; } targ_t;

static void *churn_worker(void *a) {
    targ_t *t = (targ_t *)a;
    uint8_t owner = (uint8_t)(t->tid + 1);
    uint64_t rs = 0x1234567ULL + (uint64_t)t->tid * 0x9e3779b9ULL;
    ent_t *live = calloc(LIVE, sizeof(ent_t));
    if (!live) { g_err++; return NULL; }
    long ok = 0;
    int nl = 0;
    for (int i = 0; i < t->iters; i++) {
        if (nl > 0 && (nl == LIVE || (rng(&rs) & 1))) {
            int idx = (int)(rng(&rs) % (uint64_t)nl);
            verify_and_free(&live[idx], owner);
            live[idx] = live[--nl];
            continue;
        }
        size_t req = SZS[rng(&rs) % NSZS];
        uint64_t off = sbo_alloc(g_meta, req);
        if (off == FAIL64) continue;
        ok++;
        uint64_t cap = sbo_allocated_size(g_meta, off);
        uint64_t exp = expect_cap(req);
        if (cap != exp || off + cap > DATA_SIZE || (off & 7)) {
            printf("BAD cap req=%zu off=%lu cap=%lu exp=%lu\n",
                   req, (unsigned long)off, (unsigned long)cap, (unsigned long)exp);
            g_err++; sbo_free(g_meta, off); continue;
        }
        uint64_t u0 = off / 8, un = cap / 8;
        for (uint64_t u = u0; u < u0 + un; u++) {
            uint8_t exp = 0;
            if (!atomic_compare_exchange_strong_explicit(&g_claim[u], &exp, owner,
                    memory_order_relaxed, memory_order_relaxed)) {
                printf("OVERLAP unit=%lu off=%lu held=%d\n", (unsigned long)u, (unsigned long)off, exp);
                g_err++;
            }
        }
        uint64_t tag = rng(&rs);
        memcpy(g_data + off, &tag, 8);
        memcpy(g_data + off + cap - 8, &tag, 8);
        live[nl++] = (ent_t){off, cap, tag};
    }
    for (int i = 0; i < nl; i++) verify_and_free(&live[i], owner);
    free(live);
    g_alloc_ok += ok;
    return NULL;
}

static void *allocfree_worker(void *a) {
    targ_t *t = (targ_t *)a;
    static const size_t szs[] = {8, 64, 512, 4096};
    long fail = 0;
    for (int i = 0; i < t->iters; i++) {
        uint64_t off = sbo_alloc(g_meta, szs[i & 3]);
        if (off == FAIL64) { fail++; continue; }
        sbo_free(g_meta, off);
    }
    g_phaseb_fail += fail;
    return NULL;
}

static void spawn_threads(void *(*fn)(void *), int nthr, int iters) {
    pthread_t *tids = malloc((size_t)nthr * sizeof(pthread_t));
    targ_t *args = malloc((size_t)nthr * sizeof(targ_t));
    if (!tids || !args) { g_err++; free(tids); free(args); return; }
    int created = 0;
    for (int i = 0; i < nthr; i++) {
        args[i] = (targ_t){i, iters};
        if (pthread_create(&tids[i], NULL, fn, &args[i]) != 0) { g_err++; break; }
        created++;
    }
    for (int i = 0; i < created; i++)
        if (pthread_join(tids[i], NULL) != 0) g_err++;
    free(tids); free(args);
}

static void preflight(void) {
    for (size_t i = 0; i < NSZS; i++) {
        uint64_t off = sbo_alloc(g_meta, SZS[i]);
        if (off == FAIL64) { printf("preflight alloc %zu failed\n", SZS[i]); g_err++; continue; }
        if (sbo_allocated_size(g_meta, off) != expect_cap(SZS[i])) g_err++;
        sbo_free(g_meta, off);
    }
}

static void run_phase(const char *name, void *(*fn)(void *), int nthr, int iters) {
    memset(g_meta, 0, META_SIZE);
    if (sbo_init(g_meta, META_SIZE, DATA_SIZE) != 0) { printf("%s: init fail\n", name); g_err++; return; }
    if (fn == churn_worker) preflight();
    for (uint64_t u = 0; u < UNITS; u++)
        atomic_store_explicit(&g_claim[u], 0, memory_order_relaxed);
    g_alloc_ok = 0;
    spawn_threads(fn, nthr, iters);
    if (fn == churn_worker && g_alloc_ok == 0) {
        printf("%s: no successful allocations\n", name);
        g_err++;
    }
    long leaked = 0;
    for (uint64_t u = 0; u < UNITS; u++)
        if (atomic_load_explicit(&g_claim[u], memory_order_relaxed)) leaked++;
    if (leaked) { printf("%s: leaked_units=%ld\n", name, leaked); g_err++; }
    printf("%s done (errors so far=%ld)\n", name, (long)g_err);
}

static void phase_edges(void) {
    memset(g_meta, 0, META_SIZE);
    if (sbo_init(g_meta, META_SIZE, DATA_SIZE) != 0) { printf("edges: init fail\n"); g_err++; return; }
    const size_t bad[] = {0, SIZE_MAX, SIZE_MAX - 3, 2064385};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (sbo_alloc(g_meta, bad[i]) != FAIL64) { printf("edges: alloc %zu must fail\n", bad[i]); g_err++; }
    uint64_t off = sbo_alloc(g_meta, 2064384);
    if (off == FAIL64 || sbo_allocated_size(g_meta, off) != 2064384) g_err++;
    if (off != FAIL64) sbo_free(g_meta, off);
    off = sbo_alloc(g_meta, 512);
    if (off == FAIL64) { g_err++; return; }
    if (sbo_allocated_size(g_meta, off + 8) != 0) g_err++;
    sbo_free(g_meta, off + 8);
    if (sbo_allocated_size(g_meta, off) != 512) g_err++;
    sbo_free(g_meta, off);
    off = sbo_alloc(g_meta, 8);
    if (off == FAIL64) { g_err++; return; }
    sbo_free(g_meta, off + 3);
    if (sbo_allocated_size(g_meta, off) != 8) g_err++;
    sbo_free(g_meta, off);
    if (sbo_allocated_size(g_meta, off) != 0) g_err++;
    sbo_free(g_meta, off);
    g_meta[14] = '2';
    if (sbo_alloc(g_meta, 8) != FAIL64) g_err++;
    if (sbo_allocated_size(g_meta, off) != 0) g_err++;
    sbo_free(g_meta, off);
    uint8_t *tiny = calloc(1, 8);
    if (!tiny || sbo_init(tiny, 8, DATA_SIZE) == 0) g_err++;
    free(tiny);
    printf("D edges done (errors so far=%ld)\n", (long)g_err);
}

static void phase_small_arenas(void) {
    static const size_t datas[] = {512, 32768, 2097152};
    for (size_t d = 0; d < sizeof(datas) / sizeof(datas[0]); d++) {
        size_t ms = sbo_meta_size(datas[d], 256 * 1024);
        uint8_t *m = calloc(1, ms);
        if (!m) { g_err++; continue; }
        if (sbo_init(m, ms, datas[d]) != 0) { printf("small %zu: init fail\n", datas[d]); g_err++; free(m); continue; }
        size_t maxsz = datas[d] / 64 * 63;
        uint64_t o1 = sbo_alloc(m, 504), o2 = (datas[d] > 512) ? sbo_alloc(m, 504) : 0;
        if (o1 == FAIL64 || o2 == FAIL64) g_err++;
        if (o1 != FAIL64) sbo_free(m, o1);
        if (datas[d] > 512 && o2 != FAIL64) sbo_free(m, o2);
        uint64_t off = sbo_alloc(m, maxsz);
        if (off == FAIL64 || sbo_allocated_size(m, off) != expect_cap(maxsz)) g_err++;
        if (off != FAIL64) sbo_free(m, off);
        free(m);
    }
    uint8_t *m2 = calloc(1, 1136);
    if (!m2 || sbo_init(m2, 1136, 2097152) != 0) { g_err++; free(m2); return; }
    if (sbo_alloc(m2, 8) != FAIL64) g_err++;
    uint64_t b1 = sbo_alloc(m2, 1048576), b2 = sbo_alloc(m2, 1048576);
    if (b1 == FAIL64 || b2 == FAIL64) g_err++;
    if (b1 != FAIL64) sbo_free(m2, b1);
    if (b2 != FAIL64) sbo_free(m2, b2);
    free(m2);
    uint8_t *m = calloc(1, 1 << 20);
    if (!m) { g_err++; return; }
    if (sbo_init(m, 1 << 20, 1000) == 0) g_err++;
    memset(m, 0, 1 << 20);
    if (sbo_init(m, 1 << 20, 8) == 0) g_err++;
    free(m);
    printf("E small arenas done (errors so far=%ld)\n", (long)g_err);
}

static void phase_single_shard_mt(int nthr, int iters) {
    size_t ms = sbo_meta_size(2097152, 256 * 1024);
    uint8_t *saved = g_meta;
    g_meta = calloc(1, ms);
    if (!g_meta || sbo_init(g_meta, ms, 2097152) != 0) {
        printf("single-shard: init fail\n");
        g_err++;
        free(g_meta);
        g_meta = saved;
        return;
    }
    g_phaseb_fail = 0;
    spawn_threads(allocfree_worker, nthr, iters);
    if (g_phaseb_fail) { printf("single-shard: %ld false failures\n", (long)g_phaseb_fail); g_err++; }
    free(g_meta);
    g_meta = saved;
    printf("F single-shard MT done (errors so far=%ld)\n", (long)g_err);
}

/* Single-shard arena whose box pool holds exactly `blocks` boxes (root included). */
static uint8_t *tiny_pool_arena(size_t data, uint32_t blocks) {
    size_t probe = sbo_meta_size(data, 64 * 1024);
    uint8_t *p = calloc(1, probe);
    if (!p || sbo_init(p, probe, data) != 0) { free(p); return NULL; }
    size_t stride = ((sbo_meta_t *)p)->block_stride;
    free(p);
    size_t ms = sbo_meta_size(data, blocks * stride);
    uint8_t *m = calloc(1, ms);
    if (!m || sbo_init(m, ms, data) != 0 || ((sbo_meta_t *)m)->max_blocks != blocks) {
        free(m);
        return NULL;
    }
    return m;
}

static bool overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a < b + bs && b < a + as;
}

/* With every object freed, root + (blocks-1) boxes must be rebuildable: each
 * 63*512B object needs its own L2 box. Fails if a box block leaked. */
static bool tiny_pool_recoverable(uint8_t *m, uint32_t blocks) {
    uint64_t o[64];
    uint32_t n = 0;
    bool ok = true;
    for (; n < blocks - 1 && n < 64; n++) {
        o[n] = sbo_alloc(m, 63 * 512);
        if (o[n] == FAIL64) { ok = false; break; }
    }
    for (uint32_t i = 0; i < n; i++) sbo_free(m, o[i]);
    return ok;
}

/* Pool exhausted while the search sits in an empty child box: that box must
 * not be reclaimed under the in-flight allocation (it used to be, and the new
 * object landed in a detached box overlapping later allocations). */
static void phase_tiny_pool(void) {
    const size_t data = 2097152;
    uint8_t *m = tiny_pool_arena(data, 3);
    if (!m) { printf("tiny pool: init fail\n"); g_err++; return; }
    uint64_t a = sbo_alloc(m, 63 * 512), b = sbo_alloc(m, 63 * 512);
    if (a == FAIL64 || b == FAIL64) { printf("tiny pool: setup alloc fail\n"); g_err++; free(m); return; }
    sbo_free(m, b);
    uint64_t o[3] = {a, sbo_alloc(m, 8), sbo_alloc(m, 63 * 32768)};
    const size_t req[3] = {63 * 512, 8, 63 * 32768};
    for (int i = 0; i < 3; i++) {
        if (o[i] == FAIL64) { printf("tiny pool: alloc %zu failed\n", req[i]); g_err++; continue; }
        if (sbo_allocated_size(m, o[i]) != expect_cap(req[i])) g_err++;
        for (int j = 0; j < i; j++)
            if (o[j] != FAIL64 && overlaps(o[i], expect_cap(req[i]), o[j], expect_cap(req[j]))) {
                printf("tiny pool: OVERLAP off=%lu and off=%lu\n", (unsigned long)o[i], (unsigned long)o[j]);
                g_err++;
            }
    }
    for (int i = 0; i < 3; i++) if (o[i] != FAIL64) sbo_free(m, o[i]);
    if (!tiny_pool_recoverable(m, 3)) { printf("tiny pool: box leaked\n"); g_err++; }
    free(m);
    printf("G tiny pool reclaim done (errors so far=%ld)\n", (long)g_err);
}

/* Randomized churn on a box pool small enough to be exhausted constantly. */
static void phase_tiny_pool_churn(int iters) {
    static const size_t szs[] = {8, 100, 512, 4096, 32256, 65536, 300000};
    const size_t data = 2097152;
    const uint32_t blocks = 5;
    uint8_t *m = tiny_pool_arena(data, blocks);
    if (!m) { printf("tiny churn: init fail\n"); g_err++; return; }
    uint64_t rs = 0xC0FFEEULL, off[64], cap[64];
    int nl = 0;
    long ok = 0;
    for (int i = 0; i < iters; i++) {
        if (nl > 0 && (nl == 64 || (rng(&rs) & 1))) {
            int k = (int)(rng(&rs) % (uint64_t)nl);
            if (sbo_allocated_size(m, off[k]) != cap[k]) g_err++;
            sbo_free(m, off[k]);
            off[k] = off[--nl]; cap[k] = cap[nl];
            continue;
        }
        size_t req = szs[rng(&rs) % (sizeof(szs) / sizeof(szs[0]))];
        uint64_t o = sbo_alloc(m, req);
        if (o == FAIL64) continue;
        ok++;
        uint64_t c = expect_cap(req);
        if (sbo_allocated_size(m, o) != c || o + c > data) { g_err++; sbo_free(m, o); continue; }
        for (int j = 0; j < nl; j++)
            if (overlaps(o, c, off[j], cap[j])) {
                printf("tiny churn: OVERLAP iter=%d off=%lu cap=%lu vs off=%lu cap=%lu\n", i,
                       (unsigned long)o, (unsigned long)c, (unsigned long)off[j], (unsigned long)cap[j]);
                g_err++;
                break;
            }
        off[nl] = o; cap[nl++] = c;
    }
    while (nl > 0) { nl--; sbo_free(m, off[nl]); }
    if (ok == 0) { printf("tiny churn: no successful allocations\n"); g_err++; }
    if (!tiny_pool_recoverable(m, blocks)) { printf("tiny churn: box leaked\n"); g_err++; }
    free(m);
    printf("H tiny pool churn done (errors so far=%ld)\n", (long)g_err);
}

/* Full-arena repeated failing allocs must short-circuit via no_reclaimable
 * without O(N) full-tree scans, while freeing a box re-enables lazy reclaim. */
static void phase_full_arena_oom(void) {
    const size_t data = 2097152; /* 2MB = 64 L2 slots; fill all 64 L2 boxes with 504B (63*8B) objects */
    size_t ms = sbo_meta_size(data, 2 * 1024 * 1024);
    uint8_t *m = calloc(1, ms);
    uint64_t *offs = malloc(4096 * sizeof(uint64_t));
    if (!m || !offs || sbo_init(m, ms, data) != 0) {
        printf("full oom: init fail\n");
        g_err++; free(m); free(offs); return;
    }
    /* Each L1 box holds one 504B object (slots 1..63) + one 8B object (slot 0) -> 64/64 slots used!
     * And root L2 has 64 child L1 boxes -> 4096 L1 boxes total! */
    size_t n504 = 0, n8 = 0;
    uint64_t *offs8 = malloc(4096 * sizeof(uint64_t));
    if (!offs8) { g_err++; free(m); free(offs); return; }
    while (n504 < 4096) {
        uint64_t o = sbo_alloc(m, 504);
        if (o == FAIL64) break;
        offs[n504++] = o;
    }
    while (n8 < 4096) {
        uint64_t o = sbo_alloc(m, 8);
        if (o == FAIL64) break;
        offs8[n8++] = o;
    }
    if (n504 != 4096 || n8 != 4096) {
        printf("full oom: expected 4096x504 + 4096x8, got %zu + %zu\n", n504, n8);
        g_err++;
    }
    if (sbo_alloc(m, 8) != FAIL64) g_err++; /* sets no_reclaimable = 1 */
    if (sbo_alloc(m, 512) != FAIL64) g_err++;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 10000; i++) {
        if (sbo_alloc(m, 8) != FAIL64) { g_err++; break; }
        if (sbo_alloc(m, 32768) != FAIL64) { g_err++; break; }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3
                      + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6;
    if (elapsed_ms > 500.0) {
        printf("full oom: 20000 failing allocs took %.2f ms (no_reclaimable broken?)\n", elapsed_ms);
        g_err++;
    }

    /* Free one entire L1 box [0, 512) -> clears no_reclaimable -> 512B L2 slot alloc succeeds */
    for (size_t i = 0; i < n504; i++)
        if (offs[i] < 512) sbo_free(m, offs[i]);
    for (size_t i = 0; i < n8; i++)
        if (offs8[i] < 512) sbo_free(m, offs8[i]);
    uint64_t l2_slot = sbo_alloc(m, 512);
    if (l2_slot == FAIL64) { printf("full oom: reclaim after free failed\n"); g_err++; }
    else sbo_free(m, l2_slot);
    for (size_t i = 0; i < n504; i++)
        if (offs[i] >= 512) sbo_free(m, offs[i]);
    for (size_t i = 0; i < n8; i++)
        if (offs8[i] >= 512) sbo_free(m, offs8[i]);
    free(offs8);
    free(offs);
    free(m);
    printf("I full arena OOM fast-fail done (%.2f ms, errors so far=%ld)\n", elapsed_ms, (long)g_err);
}

int main(int argc, char **argv) {
    if (argc > 3) { printf("usage: %s [threads 1..254] [iters>=1]\n", argv[0]); return 2; }
    int nthr = argc > 1 ? atoi(argv[1]) : 4;
    int iters = argc > 2 ? atoi(argv[2]) : 50000;
    if (nthr < 1 || nthr > 254 || iters < 1) {
        printf("usage: %s [threads 1..254] [iters>=1]\n", argv[0]);
        return 2;
    }
    g_meta = malloc(META_SIZE); g_data = malloc(DATA_SIZE);
    g_claim = calloc(UNITS, sizeof(_Atomic uint8_t));
    if (!g_meta || !g_data || !g_claim) { printf("out of memory\n"); return 2; }
    run_phase("A churn ST", churn_worker, 1, iters);
    run_phase("B allocfree MT (no false OOM)", allocfree_worker, nthr, iters);
    if (g_phaseb_fail) { printf("B: %ld allocations falsely failed under contention\n", (long)g_phaseb_fail); g_err++; }
    run_phase("C churn MT", churn_worker, nthr, iters);
    phase_edges();
    phase_small_arenas();
    phase_single_shard_mt(nthr, iters);
    phase_tiny_pool();
    phase_tiny_pool_churn(iters);
    phase_full_arena_oom();
    printf("18_model_check: %s\n", g_err == 0 ? "PASS" : "FAIL");
    free(g_meta); free(g_data); free(g_claim);
    return g_err == 0 ? 0 : 1;
}
