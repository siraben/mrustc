#ifndef CMRUSTC_CM_INTERNER_H
#define CMRUSTC_CM_INTERNER_H

#include "cm/arena.h"
#include "cm/map.h"
#include "cm/vec.h"

typedef uint32_t CmInternId;

#define CM_INTERN_ID_NONE ((CmInternId)0)

typedef struct CmInternedString {
    const unsigned char *bytes;
    size_t len;
} CmInternedString;

typedef struct CmInterner {
    CmArena strings;
    CmMap by_text;
    CmVec entries;
} CmInterner;

typedef struct CmInternerMark {
    const CmInterner *owner;
    CmArenaMark strings;
    size_t entry_count;
} CmInternerMark;

void cm_interner_init(CmInterner *interner, size_t arena_block_size);
void cm_interner_destroy(CmInterner *interner);
/* Interner marks inherit the arena mark lifetime and nesting contract. */
CmInternerMark cm_interner_mark(CmInterner *interner);
int cm_interner_mark_is_valid(
    const CmInterner *interner,
    CmInternerMark mark
);
void cm_interner_rewind(CmInterner *interner, CmInternerMark mark);
void cm_interner_discard_mark(CmInterner *interner, CmInternerMark mark);

CmInternId cm_interner_intern(
    CmInterner *interner,
    const void *bytes,
    size_t length
);
CmInternId cm_interner_intern_c_str(CmInterner *interner, const char *text);
CmInternId cm_interner_lookup(
    const CmInterner *interner,
    const void *bytes,
    size_t length
);
const CmInternedString *cm_interner_get(
    const CmInterner *interner,
    CmInternId id
);
size_t cm_interner_length(const CmInterner *interner);

#endif
