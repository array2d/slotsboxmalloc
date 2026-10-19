#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <blockmalloc/blockmalloc.h>
#include <slotsboxmalloc/slotsboxobj.h>

static void *meta;
static sbo_box_t *root;
static uint64_t cases;

static uint8_t longest(uint64_t bits) {
    uint8_t best = 0, run = 0;
    for (int i = 0; i < 64; i++) {
        run = bits & (UINT64_C(1) << i) ? (uint8_t)(run + 1) : 0;
        if (run > best) best = run;
    }
    return best;
}

static int rightmost(uint64_t bits, int boundary, int count) {
    for (int end = 63; end >= boundary; end--) {
        int start = end - count + 1;
        if (start < boundary) break;
        int free_run = 1;
        for (int i = start; i <= end; i++) {
            if (!(bits & (UINT64_C(1) << i))) { free_run = 0; break; }
        }
        if (free_run) return start;
    }
    return -1;
}

static void fail(uint64_t bits, int boundary, int count, const char *what) {
    fprintf(stderr, "%s: bits=%016" PRIx64 " boundary=%d count=%d\n",
            what, bits, boundary, count);
    exit(1);
}

static void check_with_hint(uint64_t bits, int boundary, int count, sbo_usage_t hint) {
    root->box_boundary = (int16_t)boundary;
    root->obj_boundary = 63;
    root->max_obj_cap = longest(bits);
    root->child_max_cap = hint;
    for (int i = 0; i < 8; i++) root->free_bitmap[i] = (uint8_t)(bits >> (8 * i));
    for (int i = 0; i < 64; i++)
        root->slots[i].state = bits & (UINT64_C(1) << i) ? SBO_FREE : SBO_OBJ_START;

    sbo_box_t expected = *root;
    int start = rightmost(bits, boundary, count);
    uint64_t offset = sbo_alloc(meta, (size_t)count * 8);
    uint64_t want = start < 0 ? UINT64_MAX : (uint64_t)start * 8;
    if (offset != want) fail(bits, boundary, count, "rightmost placement");
    if (start >= 0) {
        uint64_t mask = (UINT64_MAX >> (64 - count)) << start;
        for (int i = start; i < start + count; i++) {
            expected.free_bitmap[i >> 3] &= (uint8_t)~(1U << (i & 7));
            expected.slots[i].state = i == start ? SBO_OBJ_START : SBO_OBJ_CONT;
        }
        expected.obj_boundary = (int16_t)(start - 1);
        expected.max_obj_cap = longest(bits & ~mask);
        if (sbo_allocated_size(meta, offset) != (uint64_t)count * 8)
            fail(bits, boundary, count, "allocated size");
    }
    if (memcmp(root, &expected, sizeof expected)) fail(bits, boundary, count, "allocation state");
    if (start >= 0) {
        sbo_free(meta, offset);
        for (int i = start; i < start + count; i++) {
            expected.free_bitmap[i >> 3] |= (uint8_t)(1U << (i & 7));
            expected.slots[i].state = SBO_FREE;
        }
        expected.max_obj_cap = longest(bits);
        if (memcmp(root, &expected, sizeof expected)) fail(bits, boundary, count, "free state");
    }
    cases++;
}

static void all_counts(uint64_t bits) {
    sbo_usage_t zero_hint = {0, 0};
    for (int boundary = 0; boundary <= 64; boundary++)
        for (int count = 1; count <= 63; count++)
            check_with_hint(bits, boundary, count, zero_hint);
}

static void check_multilevel_propagation(void) {
    /* 2-level pool (32KB = 8 * 64^2): verify depth-1 child boxes (parent == 0)
     * propagate capacity changes to root and seamlessly spill to sibling boxes. */
    size_t data_size = 8ULL * 64 * 64;
    size_t head_size = sbo_meta_size(data_size, 64 * 1024);
    void *m2 = calloc(1, head_size);
    if (!m2 || sbo_init(m2, head_size, data_size) != 0) {
        fprintf(stderr, "multilevel init failed\n");
        exit(1);
    }
    uint64_t offs[128];
    for (int i = 0; i < 128; i++) {
        offs[i] = sbo_alloc(m2, 8);
        if (offs[i] == UINT64_MAX || sbo_allocated_size(m2, offs[i]) != 8) {
            fprintf(stderr, "multilevel alloc failed at %d\n", i);
            exit(1);
        }
    }
    for (int i = 0; i < 128; i += 2) sbo_free(m2, offs[i]);
    for (int i = 0; i < 128; i += 2) {
        uint64_t re = sbo_alloc(m2, 8);
        if (re == UINT64_MAX) {
            fprintf(stderr, "multilevel realloc failed at %d\n", i);
            exit(1);
        }
        offs[i] = re;
    }
    for (int i = 0; i < 128; i++) sbo_free(m2, offs[i]);
    free(m2);
}

int main(void) {
    meta = calloc(1, 4096);
    if (!meta || sbo_init(meta, 4096, 512) != 0) return 1;
    sbo_meta_t *m = meta;
    root = (sbo_box_t *)((uint8_t *)(m + 1)
            + m->root_slots * (sizeof(blocks_meta_t) + sizeof(sbo_lock_t))
            + m->sizeof_block_head);

    /* Exercise leaf capacity guard even when subtree hint is optimistic. */
    sbo_usage_t opt_hint = {2, 1};
    for (uint64_t bits = 0; bits < 256; bits++) {
        for (int shift = 0; shift <= 56; shift += 28)
            for (int boundary = 0; boundary <= 64; boundary++)
                for (int count = 1; count <= 9; count++)
                    check_with_hint(bits << shift, boundary, count, opt_hint);
    }
    all_counts(0);
    all_counts(UINT64_MAX);
    all_counts(UINT64_C(0xAAAAAAAAAAAAAAAA));
    all_counts(UINT64_C(0x5555555555555555));
    for (int bit = 0; bit < 64; bit++) {
        all_counts(UINT64_C(1) << bit);
        all_counts(~(UINT64_C(1) << bit));
    }
    uint64_t seed = UINT64_C(0x937bf272d9ea1563);
    for (int i = 0; i < 64; i++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        uint64_t bits = seed;
        if (i % 3 == 0) bits &= UINT64_C(0x5555555555555555);
        else if (i % 3 == 1) bits |= UINT64_C(0xAAAAAAAAAAAAAAAA);
        all_counts(bits);
    }
    free(meta);

    check_multilevel_propagation();

    printf("PASS bitmap regression: %" PRIu64 " cases\n", cases);
    return 0;
}
