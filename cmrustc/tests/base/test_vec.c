#include "cm/vec.h"

#include <assert.h>
#include <stddef.h>

typedef struct IntVec {
    CmVec raw;
} IntVec;

static void int_vec_init(IntVec *vector)
{
    cm_vec_init(&vector->raw, sizeof(int));
}

static int *int_vec_at(IntVec *vector, size_t index)
{
    return (int *)cm_vec_at(&vector->raw, index);
}

static void int_vec_push(IntVec *vector, int value)
{
    (void)cm_vec_push(&vector->raw, &value);
}

int main(void)
{
    IntVec vector;
    int popped;
    int *slot;
    size_t index;

    int_vec_init(&vector);
    for (index = 0; index < 64; index += 1) {
        int_vec_push(&vector, (int)(index * 3));
    }
    assert(vector.raw.len == 64);
    for (index = 0; index < vector.raw.len; index += 1) {
        assert(*int_vec_at(&vector, index) == (int)(index * 3));
    }
    assert(cm_vec_at(&vector.raw, vector.raw.len) == NULL);

    cm_vec_append(&vector.raw, int_vec_at(&vector, 8), 16);
    assert(vector.raw.len == 80);
    for (index = 0; index < 16; index += 1) {
        assert(*int_vec_at(&vector, 64 + index) == (int)((8 + index) * 3));
    }

    cm_vec_resize(&vector.raw, 100);
    for (index = 80; index < 100; index += 1) {
        assert(*int_vec_at(&vector, index) == 0);
    }
    slot = (int *)cm_vec_push_uninit(&vector.raw);
    *slot = 90210;
    assert(*int_vec_at(&vector, 100) == 90210);
    assert(cm_vec_pop(&vector.raw, &popped));
    assert(popped == 90210);
    assert(vector.raw.len == 100);

    cm_vec_clear(&vector.raw);
    assert(vector.raw.len == 0);
    assert(!cm_vec_pop(&vector.raw, &popped));
    cm_vec_destroy(&vector.raw);
    return 0;
}
