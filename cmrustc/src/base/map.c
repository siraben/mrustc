#include "cm/map.h"

#include "cm/alloc.h"

#include <stdlib.h>
#include <string.h>

enum CmMapSlotState {
    CM_MAP_SLOT_EMPTY = 0,
    CM_MAP_SLOT_OCCUPIED = 1,
    CM_MAP_SLOT_TOMBSTONE = 2
};

struct CmMapSlot {
    uint64_t hash;
    unsigned char *storage;
    size_t key_len;
    unsigned char state;
};

static const unsigned char *cm_map_slot_key(
    const CmMap *map,
    const CmMapSlot *slot
)
{
    return slot->storage + map->value_size;
}

static int cm_map_key_is_valid(const void *key, size_t key_length)
{
    return key != NULL || key_length == 0;
}

uint64_t cm_hash_bytes(const void *bytes, size_t length)
{
    const unsigned char *input;
    uint64_t hash;
    size_t index;

    if (!cm_map_key_is_valid(bytes, length)) {
        abort();
    }
    input = (const unsigned char *)bytes;
    hash = UINT64_C(14695981039346656037);
    for (index = 0; index < length; index += 1) {
        hash ^= (uint64_t)input[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int cm_map_slot_matches(
    const CmMap *map,
    const CmMapSlot *slot,
    uint64_t hash,
    const void *key,
    size_t key_length
)
{
    if (slot->state != CM_MAP_SLOT_OCCUPIED
        || slot->hash != hash
        || slot->key_len != key_length) {
        return 0;
    }
    return key_length == 0
        || memcmp(cm_map_slot_key(map, slot), key, key_length) == 0;
}

static CmMapSlot *cm_map_find_slot(
    const CmMap *map,
    uint64_t hash,
    const void *key,
    size_t key_length,
    int for_insert,
    int *found
)
{
    CmMapSlot *slots;
    CmMapSlot *slot;
    CmMapSlot *first_tombstone;
    size_t index;
    size_t visited;

    *found = 0;
    if (map->cap == 0) {
        return NULL;
    }
    slots = map->slots;
    index = (size_t)hash & (map->cap - 1);
    first_tombstone = NULL;
    for (visited = 0; visited < map->cap; visited += 1) {
        slot = &slots[index];
        if (slot->state == CM_MAP_SLOT_EMPTY) {
            return for_insert && first_tombstone != NULL
                ? first_tombstone
                : slot;
        }
        if (slot->state == CM_MAP_SLOT_TOMBSTONE) {
            if (first_tombstone == NULL) {
                first_tombstone = slot;
            }
        } else if (cm_map_slot_matches(
            map,
            slot,
            hash,
            key,
            key_length
        )) {
            *found = 1;
            return slot;
        }
        index = (index + 1) & (map->cap - 1);
    }
    return for_insert ? first_tombstone : NULL;
}

static size_t cm_map_max_load(size_t capacity)
{
    return capacity - capacity / 4;
}

static void cm_map_place_existing(
    CmMapSlot *slots,
    size_t capacity,
    CmMapSlot old_slot
)
{
    size_t index;

    index = (size_t)old_slot.hash & (capacity - 1);
    while (slots[index].state == CM_MAP_SLOT_OCCUPIED) {
        index = (index + 1) & (capacity - 1);
    }
    slots[index] = old_slot;
}

static void cm_map_rehash(CmMap *map, size_t capacity)
{
    CmMapSlot *new_slots;
    size_t index;
    size_t allocation_size;

    if (!cm_size_mul(capacity, sizeof(*new_slots), &allocation_size)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    new_slots = (CmMapSlot *)cm_alloc_zeroed(1, allocation_size);
    for (index = 0; index < map->cap; index += 1) {
        if (map->slots[index].state == CM_MAP_SLOT_OCCUPIED) {
            cm_map_place_existing(new_slots, capacity, map->slots[index]);
        }
    }
    cm_free(map->slots);
    map->slots = new_slots;
    map->cap = capacity;
    map->tombstones = 0;
}

void cm_map_init(CmMap *map, size_t value_size)
{
    map->slots = NULL;
    map->len = 0;
    map->tombstones = 0;
    map->cap = 0;
    map->value_size = value_size;
}

void cm_map_destroy(CmMap *map)
{
    size_t value_size;
    size_t index;

    value_size = map->value_size;
    for (index = 0; index < map->cap; index += 1) {
        if (map->slots[index].state == CM_MAP_SLOT_OCCUPIED) {
            cm_free(map->slots[index].storage);
        }
    }
    cm_free(map->slots);
    cm_map_init(map, value_size);
}

void cm_map_clear(CmMap *map)
{
    size_t index;

    for (index = 0; index < map->cap; index += 1) {
        if (map->slots[index].state == CM_MAP_SLOT_OCCUPIED) {
            cm_free(map->slots[index].storage);
        }
    }
    if (map->slots != NULL) {
        memset(map->slots, 0, map->cap * sizeof(*map->slots));
    }
    map->len = 0;
    map->tombstones = 0;
}

void cm_map_reserve(CmMap *map, size_t minimum_entries)
{
    size_t capacity;

    capacity = map->cap == 0 ? 8 : map->cap;
    while (minimum_entries > cm_map_max_load(capacity)) {
        if (capacity > (size_t)-1 / 2) {
            cm_alloc_out_of_memory((size_t)-1);
        }
        capacity *= 2;
    }
    if (capacity != map->cap || map->tombstones != 0) {
        cm_map_rehash(map, capacity);
    }
}

static const void *cm_map_get_inner(
    const CmMap *map,
    const void *key,
    size_t key_length
)
{
    CmMapSlot *slot;
    uint64_t hash;
    int found;

    if (!cm_map_key_is_valid(key, key_length)) {
        abort();
    }
    hash = cm_hash_bytes(key, key_length);
    slot = cm_map_find_slot(map, hash, key, key_length, 0, &found);
    return found ? slot->storage : NULL;
}

void *cm_map_get(CmMap *map, const void *key, size_t key_length)
{
    return (void *)cm_map_get_inner(map, key, key_length);
}

const void *cm_map_get_const(
    const CmMap *map,
    const void *key,
    size_t key_length
)
{
    return cm_map_get_inner(map, key, key_length);
}

void *cm_map_insert(
    CmMap *map,
    const void *key,
    size_t key_length,
    const void *value,
    int *inserted
)
{
    CmMapSlot *slot;
    unsigned char *storage;
    uint64_t hash;
    size_t storage_size;
    size_t used;
    size_t new_capacity;
    int found;

    if (!cm_map_key_is_valid(key, key_length)
        || (map->value_size != 0 && value == NULL)) {
        abort();
    }
    hash = cm_hash_bytes(key, key_length);
    slot = cm_map_find_slot(map, hash, key, key_length, 0, &found);
    if (found) {
        if (map->value_size != 0) {
            memmove(slot->storage, value, map->value_size);
        }
        if (inserted != NULL) {
            *inserted = 0;
        }
        return slot->storage;
    }

    if (!cm_size_add(map->len, map->tombstones, &used)
        || !cm_size_add(used, 1, &used)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    if (map->cap == 0 || used > cm_map_max_load(map->cap)) {
        new_capacity = map->cap == 0 ? 8 : map->cap;
        if (map->len + 1 > cm_map_max_load(new_capacity)) {
            if (new_capacity > (size_t)-1 / 2) {
                cm_alloc_out_of_memory((size_t)-1);
            }
            new_capacity *= 2;
        }
        cm_map_rehash(map, new_capacity);
    }

    slot = cm_map_find_slot(map, hash, key, key_length, 1, &found);
    if (slot == NULL || found) {
        abort();
    }
    if (!cm_size_add(map->value_size, key_length, &storage_size)) {
        cm_alloc_out_of_memory((size_t)-1);
    }
    storage = (unsigned char *)cm_alloc(storage_size);
    if (map->value_size != 0) {
        memcpy(storage, value, map->value_size);
    }
    if (key_length != 0) {
        memcpy(storage + map->value_size, key, key_length);
    }

    if (slot->state == CM_MAP_SLOT_TOMBSTONE) {
        map->tombstones -= 1;
    }
    slot->hash = hash;
    slot->storage = storage;
    slot->key_len = key_length;
    slot->state = CM_MAP_SLOT_OCCUPIED;
    map->len += 1;
    if (inserted != NULL) {
        *inserted = 1;
    }
    return storage;
}

int cm_map_remove(
    CmMap *map,
    const void *key,
    size_t key_length,
    void *value_out
)
{
    CmMapSlot *slot;
    uint64_t hash;
    int found;

    if (!cm_map_key_is_valid(key, key_length)) {
        abort();
    }
    hash = cm_hash_bytes(key, key_length);
    slot = cm_map_find_slot(map, hash, key, key_length, 0, &found);
    if (!found) {
        return 0;
    }
    if (value_out != NULL && map->value_size != 0) {
        memcpy(value_out, slot->storage, map->value_size);
    }
    cm_free(slot->storage);
    slot->storage = NULL;
    slot->key_len = 0;
    slot->state = CM_MAP_SLOT_TOMBSTONE;
    map->len -= 1;
    map->tombstones += 1;
    return 1;
}

size_t cm_map_length(const CmMap *map)
{
    return map->len;
}

size_t cm_map_capacity(const CmMap *map)
{
    return map->cap;
}

size_t cm_map_tombstone_count(const CmMap *map)
{
    return map->tombstones;
}
