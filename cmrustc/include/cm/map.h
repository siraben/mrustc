#ifndef CMRUSTC_CM_MAP_H
#define CMRUSTC_CM_MAP_H

#include "cm/config.h"

typedef struct CmMapSlot CmMapSlot;

typedef struct CmMap {
    CmMapSlot *slots;
    size_t len;
    size_t tombstones;
    size_t cap;
    size_t value_size;
} CmMap;

/* Stable 64-bit FNV-1a over exactly length bytes. */
uint64_t cm_hash_bytes(const void *bytes, size_t length);

void cm_map_init(CmMap *map, size_t value_size);
void cm_map_destroy(CmMap *map);
void cm_map_clear(CmMap *map);
void cm_map_reserve(CmMap *map, size_t minimum_entries);

void *cm_map_get(CmMap *map, const void *key, size_t key_length);
const void *cm_map_get_const(
    const CmMap *map,
    const void *key,
    size_t key_length
);

/* Returns the stored value and sets inserted to one only for a new key. */
void *cm_map_insert(
    CmMap *map,
    const void *key,
    size_t key_length,
    const void *value,
    int *inserted
);
int cm_map_remove(
    CmMap *map,
    const void *key,
    size_t key_length,
    void *value_out
);

size_t cm_map_length(const CmMap *map);
size_t cm_map_capacity(const CmMap *map);
size_t cm_map_tombstone_count(const CmMap *map);

#endif
