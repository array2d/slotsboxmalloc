#define BLOCKMALLOC_IMPLEMENTATION
#include <blockmalloc/blockmalloc.h>
#define SLOTSBOXMALLOC_IMPLEMENTATION
#include <slotsboxmalloc/slotsboxobj.h>

int main(void) {
    sbo_box_t box;
    sbo_box_format(NULL, &box, 1, 0);
    if (box.child_max_cap.level != 0 || box.child_max_cap.multiple != 0)
        return 1;

    for (int i = 0; i < SBO_N; i++) {
        if (sbo_alloc_obj_slots(NULL, &box, 1) < 0)
            return 2;
    }
    if (sbo_usage_cmp(sbo_box_and_child_max_cap(&box), (sbo_usage_t){0, 0}) != 0)
        return 3;

    sbo_free_obj_slots(NULL, &box, 0);
    if (sbo_usage_cmp(sbo_box_and_child_max_cap(&box), (sbo_usage_t){1, 1}) != 0)
        return 4;
    return 0;
}
