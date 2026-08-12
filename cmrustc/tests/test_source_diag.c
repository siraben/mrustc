#include "cm/diag.h"
#include "cm/source.h"

#include <stdio.h>
#include <string.h>

static int check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test-source-diag: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void)
{
    static const unsigned char first[] = "one\ntwo\n";
    static const unsigned char second[] = "alpha\nbeta";
    CmSourceSet sources;
    CmSourceId first_id;
    CmSourceId second_id;
    CmSpan span;
    uint32_t line;
    uint32_t column;
    CmSourceId bounded_id;
    int ok;

    cm_source_set_init(&sources);
    ok = 1;
    ok &= check(cm_source_add_memory(&sources, "first.rs", first,
        sizeof(first) - 1u, &first_id) == CM_SOURCE_OK,
        "failed to add first source");
    ok &= check(cm_source_add_memory(&sources, "second.rs", second,
        sizeof(second) - 1u, &second_id) == CM_SOURCE_OK,
        "failed to add second source");
    ok &= check(first_id != second_id && sources.length == 2u,
        "source IDs are not stable and distinct");

    span.source = second_id;
    span.start = 8u;
    span.end = 8u;
    ok &= check(cm_source_line_column(&sources, span, &line, &column),
        "valid span rejected");
    ok &= check(line == 2u && column == 3u,
        "incorrect line or column");
    span.end = 99u;
    ok &= check(!cm_span_is_valid(&sources, span),
        "out-of-bounds span accepted");
    ok &= check(cm_source_load_file_bounded(&sources,
        "tests/fixtures/bounded-source.rs", 9u, &bounded_id)
        == CM_SOURCE_OK && sources.length == 3u
        && cm_source_get(&sources, bounded_id) != NULL
        && cm_source_get(&sources, bounded_id)->length == 9u,
        "exact bounded source limit was rejected");
    ok &= check(cm_source_load_file_bounded(&sources,
        "tests/fixtures/bounded-source.rs", 8u, &bounded_id)
        == CM_SOURCE_TOO_LARGE && sources.length == 3u,
        "one-byte-over source was not rejected atomically");

    if (ok) {
        span.end = span.start;
        cm_diag_emit(stdout, &sources, CM_DIAG_NOTE, span, "snapshot");
    }
    cm_source_set_destroy(&sources);
    return ok ? 0 : 1;
}
