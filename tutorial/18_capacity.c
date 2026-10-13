#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <slotsboxmalloc/slotsboxobj.h>

static int exercise(void *meta, uint8_t *data, size_t data_size) {
    uint64_t offsets[190];
    size_t sizes[190];
    int count = 0;
    size_t size = data_size;
    for (int depth = 0; depth < 3; depth++) {
        size /= 64;
        int n = depth == 2 ? 64 : 63;
        for (int i = 0; i < n; i++) {
            uint64_t off = sbo_alloc(meta, size);
            if (off == UINT64_MAX || off + size > data_size ||
                sbo_allocated_size(meta, off) != size)
                return 2;
            for (int j = 0; j < count; j++) {
                if (off < offsets[j] + sizes[j] && offsets[j] < off + size)
                    return 3;
            }
            offsets[count] = off;
            sizes[count] = size;
            data[off] = data[off + size - 1] = (uint8_t)count;
            count++;
        }
    }
    if (sbo_alloc(meta, size) != UINT64_MAX)
        return 4;

    int hole = 143;
    sbo_free(meta, offsets[hole]);
    uint64_t replacement = sbo_alloc(meta, sizes[hole]);
    if (replacement != offsets[hole])
        return 5;
    for (int i = 0; i < count; i++) {
        if (i != hole && (data[offsets[i]] != (uint8_t)i ||
                         data[offsets[i] + sizes[i] - 1] != (uint8_t)i))
            return 6;
        sbo_free(meta, offsets[i]);
    }
    return 0;
}

int main(void) {
    size_t data_size = 8ULL * 64 * 64 * 64 * 64;
    size_t head_size = sbo_meta_size(data_size, 256 * 1024);
    void *meta = malloc(head_size);
    uint8_t *data = calloc(1, data_size);
    int rc = 1;
    if (meta)
        memset(meta, 0xa5, head_size);
    if (meta && data && sbo_init(meta, head_size, data_size) == 0)
        rc = exercise(meta, data, data_size);
    free(data);
    free(meta);
    if (rc == 0)
        puts("capacity and reuse passed");
    return rc;
}
