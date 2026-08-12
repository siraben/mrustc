#include "cm/alloc.h"
#include "cm/map.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static CmMap fault_map;
static jmp_buf oom_jump;
static int oom_seen;

static void jump_on_oom(size_t requested_size, void *context)
{
    (void)requested_size;
    (void)context;
    oom_seen += 1;
    longjmp(oom_jump, 1);
}

static void test_fault_behavior(void)
{
    int inserted;
    int value;

    value = 17;
    cm_map_init(&fault_map, sizeof(value));
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0);
    if (setjmp(oom_jump) == 0) {
        (void)cm_map_insert(&fault_map, "first", 5, &value, &inserted);
        assert(0);
    }
    cm_alloc_fail_never();
    assert(cm_map_length(&fault_map) == 0);
    assert(cm_map_capacity(&fault_map) == 0);

    cm_map_reserve(&fault_map, 1);
    cm_alloc_fail_after(0);
    if (setjmp(oom_jump) == 0) {
        (void)cm_map_insert(&fault_map, "second", 6, &value, &inserted);
        assert(0);
    }
    cm_alloc_fail_never();
    assert(cm_map_length(&fault_map) == 0);
    assert(cm_map_get(&fault_map, "second", 6) == NULL);

    (void)cm_map_insert(&fault_map, "second", 6, &value, &inserted);
    assert(inserted);
    assert(*(int *)cm_map_get(&fault_map, "second", 6) == value);
    cm_map_destroy(&fault_map);
    cm_alloc_set_oom_handler(NULL, NULL);
    assert(oom_seen == 2);
}

static size_t find_colliding_keys(char keys[4][24])
{
    unsigned counts[8];
    char candidates[8][4][24];
    char candidate[24];
    size_t bucket;
    size_t index;
    unsigned candidate_number;

    memset(counts, 0, sizeof(counts));
    for (candidate_number = 0; candidate_number < 10000; candidate_number += 1) {
        (void)snprintf(
            candidate,
            sizeof(candidate),
            "collision-%u",
            candidate_number
        );
        bucket = (size_t)cm_hash_bytes(candidate, strlen(candidate)) & 7;
        if (counts[bucket] < 4) {
            (void)strcpy(candidates[bucket][counts[bucket]], candidate);
            counts[bucket] += 1;
            if (counts[bucket] == 4) {
                for (index = 0; index < 4; index += 1) {
                    (void)strcpy(keys[index], candidates[bucket][index]);
                }
                return bucket;
            }
        }
    }
    assert(0);
    return 0;
}

int main(void)
{
    CmMap map;
    char collision_keys[4][24];
    char key[32];
    unsigned char binary_a[] = { 'a', 0, 'b' };
    unsigned char binary_b[] = { 'a', 0, 'c' };
    int inserted;
    int removed;
    int value;
    int replacement;
    size_t index;
    size_t initial_capacity;

    assert(cm_hash_bytes(NULL, 0) == UINT64_C(14695981039346656037));
    assert(cm_hash_bytes("a", 1) == UINT64_C(12638187200555641996));

    cm_map_init(&map, sizeof(value));
    (void)find_colliding_keys(collision_keys);
    for (index = 0; index < 3; index += 1) {
        value = (int)(100 + index);
        (void)cm_map_insert(
            &map,
            collision_keys[index],
            strlen(collision_keys[index]),
            &value,
            &inserted
        );
        assert(inserted);
    }
    assert(cm_map_length(&map) == 3);
    assert(cm_map_capacity(&map) == 8);
    assert(cm_map_remove(
        &map,
        collision_keys[1],
        strlen(collision_keys[1]),
        &removed
    ));
    assert(removed == 101);
    assert(cm_map_tombstone_count(&map) == 1);
    assert(*(int *)cm_map_get(
        &map,
        collision_keys[2],
        strlen(collision_keys[2])
    ) == 102);

    value = 103;
    (void)cm_map_insert(
        &map,
        collision_keys[3],
        strlen(collision_keys[3]),
        &value,
        &inserted
    );
    assert(inserted);
    assert(cm_map_tombstone_count(&map) == 0);

    value = 200;
    (void)cm_map_insert(&map, binary_a, sizeof(binary_a), &value, &inserted);
    assert(inserted);
    value = 201;
    (void)cm_map_insert(&map, binary_b, sizeof(binary_b), &value, &inserted);
    assert(inserted);
    value = 202;
    (void)cm_map_insert(&map, "a", 1, &value, &inserted);
    assert(inserted);
    value = 203;
    (void)cm_map_insert(&map, NULL, 0, &value, &inserted);
    assert(inserted);
    assert(*(int *)cm_map_get(&map, binary_a, sizeof(binary_a)) == 200);
    assert(*(int *)cm_map_get(&map, binary_b, sizeof(binary_b)) == 201);
    assert(*(int *)cm_map_get(&map, "a", 1) == 202);
    assert(*(int *)cm_map_get(&map, NULL, 0) == 203);

    replacement = 999;
    (void)cm_map_insert(
        &map,
        binary_a,
        sizeof(binary_a),
        &replacement,
        &inserted
    );
    assert(!inserted);
    assert(*(int *)cm_map_get(&map, binary_a, sizeof(binary_a)) == replacement);

    initial_capacity = cm_map_capacity(&map);
    for (index = 0; index < 300; index += 1) {
        (void)snprintf(key, sizeof(key), "bulk-%lu", (unsigned long)index);
        value = (int)index;
        (void)cm_map_insert(&map, key, strlen(key), &value, &inserted);
        assert(inserted);
    }
    assert(cm_map_capacity(&map) > initial_capacity);
    for (index = 0; index < 300; index += 1) {
        const int *stored;

        (void)snprintf(key, sizeof(key), "bulk-%lu", (unsigned long)index);
        stored = (const int *)cm_map_get_const(&map, key, strlen(key));
        assert(stored != NULL);
        assert(*stored == (int)index);
    }

    cm_map_clear(&map);
    assert(cm_map_length(&map) == 0);
    assert(cm_map_tombstone_count(&map) == 0);
    assert(cm_map_get(&map, binary_a, sizeof(binary_a)) == NULL);
    cm_map_destroy(&map);

    test_fault_behavior();
    return 0;
}
