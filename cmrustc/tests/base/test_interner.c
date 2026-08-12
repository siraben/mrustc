#include "cm/alloc.h"
#include "cm/interner.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static CmInterner fault_interner;
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
    CmInternId seed;
    CmInternId recovered;

    cm_interner_init(&fault_interner, 32);
    seed = cm_interner_intern_c_str(&fault_interner, "seed");
    assert(seed == 1);

    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0);
    if (setjmp(oom_jump) == 0) {
        (void)cm_interner_intern_c_str(&fault_interner, "will-fail");
        assert(0);
    }
    cm_alloc_fail_never();
    assert(cm_interner_length(&fault_interner) == 1);
    assert(cm_interner_lookup(&fault_interner, "will-fail", 9) == 0);

    recovered = cm_interner_intern_c_str(&fault_interner, "will-fail");
    assert(recovered == 2);
    assert(cm_interner_lookup(&fault_interner, "will-fail", 9) == recovered);
    cm_alloc_set_oom_handler(NULL, NULL);
    cm_interner_destroy(&fault_interner);
    assert(oom_seen == 1);
}

static void test_mark_generations(void)
{
    CmInterner interner;
    CmInternerMark outer;
    CmInternerMark stale_inner;
    CmInternerMark current;
    CmInternerMark stale_lifetime;
    CmInternId replacement;

    cm_interner_init(&interner, 64);
    assert(cm_interner_intern_c_str(&interner, "seed") == 1u);
    outer = cm_interner_mark(&interner);
    assert(cm_interner_intern_c_str(&interner, "x") == 2u);
    stale_inner = cm_interner_mark(&interner);

    cm_interner_rewind(&interner, outer);
    replacement = cm_interner_intern_c_str(&interner, "replacement");
    assert(replacement == 2u);
    assert(!cm_interner_mark_is_valid(&interner, stale_inner));
    assert(cm_interner_mark_is_valid(&interner, outer));
    assert(cm_interner_length(&interner) == 2u);
    assert(cm_interner_lookup(&interner, "replacement", 11u)
        == replacement);
    assert(cm_interner_lookup(&interner, "x", 1u) == CM_INTERN_ID_NONE);

    current = cm_interner_mark(&interner);
    cm_interner_discard_mark(&interner, current);
    assert(!cm_interner_mark_is_valid(&interner, current));
    cm_interner_rewind(&interner, outer);
    assert(cm_interner_lookup(&interner, "replacement", 11u)
        == CM_INTERN_ID_NONE);
    cm_interner_discard_mark(&interner, outer);
    assert(!cm_interner_mark_is_valid(&interner, outer));

    stale_lifetime = cm_interner_mark(&interner);
    cm_interner_destroy(&interner);
    cm_interner_init(&interner, 64);
    assert(!cm_interner_mark_is_valid(&interner, stale_lifetime));
    cm_interner_destroy(&interner);
}

int main(void)
{
    CmInterner interner;
    CmInternerMark empty_mark;
    CmInternerMark stable_mark;
    CmInternerMark nested_mark;
    CmInternId empty;
    CmInternId alpha;
    CmInternId alpha_again;
    CmInternId binary;
    CmInternId binary_other;
    const CmInternedString *stored;
    const unsigned char *stable_pointer;
    unsigned char bytes[] = { 'x', 0, 'y' };
    unsigned char other[] = { 'x', 0, 'z' };
    char text[32];
    size_t index;

    cm_interner_init(&interner, 32);
    empty_mark = cm_interner_mark(&interner);
    empty = cm_interner_intern(&interner, NULL, 0);
    alpha = cm_interner_intern_c_str(&interner, "alpha");
    alpha_again = cm_interner_intern(&interner, "alpha", 5);
    stable_mark = cm_interner_mark(&interner);
    binary = cm_interner_intern(&interner, bytes, sizeof(bytes));
    binary_other = cm_interner_intern(&interner, other, sizeof(other));
    nested_mark = cm_interner_mark(&interner);

    assert(empty != CM_INTERN_ID_NONE);
    assert(alpha != CM_INTERN_ID_NONE);
    assert(alpha == alpha_again);
    assert(binary != binary_other);
    assert(cm_interner_length(&interner) == 4);
    assert(cm_interner_lookup(&interner, bytes, sizeof(bytes)) == binary);
    assert(cm_interner_lookup(&interner, "missing", 7) == 0);

    stored = cm_interner_get(&interner, binary);
    assert(stored != NULL);
    assert(stored->len == sizeof(bytes));
    assert(memcmp(stored->bytes, bytes, sizeof(bytes)) == 0);
    assert(stored->bytes[sizeof(bytes)] == 0);
    stable_pointer = stored->bytes;

    for (index = 0; index < 2000; index += 1) {
        CmInternId id;

        (void)snprintf(text, sizeof(text), "identifier-%lu", (unsigned long)index);
        id = cm_interner_intern_c_str(&interner, text);
        assert(id != CM_INTERN_ID_NONE);
        assert(cm_interner_lookup(&interner, text, strlen(text)) == id);
    }
    stored = cm_interner_get(&interner, binary);
    assert(stored != NULL);
    assert(stored->bytes == stable_pointer);
    assert(memcmp(stored->bytes, bytes, sizeof(bytes)) == 0);
    assert(cm_interner_get(&interner, CM_INTERN_ID_NONE) == NULL);
    assert(cm_interner_get(&interner, (CmInternId)UINT32_MAX) == NULL);

    cm_interner_rewind(&interner, nested_mark);
    assert(cm_interner_length(&interner) == 4);
    assert(cm_interner_get(&interner, binary) != NULL);
    assert(cm_interner_lookup(&interner, "identifier-0", 12) == 0);

    cm_interner_rewind(&interner, stable_mark);
    assert(cm_interner_length(&interner) == 2);
    assert(cm_interner_lookup(&interner, "alpha", 5) == alpha);
    assert(cm_interner_lookup(&interner, bytes, sizeof(bytes)) == 0);
    binary = cm_interner_intern(&interner, bytes, sizeof(bytes));
    assert(binary == 3);
    assert(cm_interner_get(&interner, empty) != NULL);
    assert(cm_interner_get(&interner, alpha) != NULL);

    cm_interner_rewind(&interner, empty_mark);
    assert(cm_interner_length(&interner) == 0);
    assert(cm_interner_lookup(&interner, "alpha", 5) == 0);
    assert(cm_interner_lookup(&interner, bytes, sizeof(bytes)) == 0);
    assert(cm_interner_intern_c_str(&interner, "fresh") == 1);

    cm_interner_destroy(&interner);
    test_fault_behavior();
    test_mark_generations();
    return 0;
}
