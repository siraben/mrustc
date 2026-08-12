#ifndef CM_SOURCE_H
#define CM_SOURCE_H

#include "cm/config.h"

typedef uint32_t CmSourceId;

typedef struct CmSpan {
    CmSourceId source;
    uint32_t start;
    uint32_t end;
} CmSpan;

typedef enum CmSourceStatus {
    CM_SOURCE_OK = 0,
    CM_SOURCE_OUT_OF_MEMORY,
    CM_SOURCE_IO_ERROR,
    CM_SOURCE_TOO_LARGE,
    CM_SOURCE_INVALID_ARGUMENT
} CmSourceStatus;

typedef struct CmSourceFile {
    CmSourceId id;
    char *path;
    unsigned char *bytes;
    size_t length;
    uint32_t *line_starts;
    size_t line_count;
} CmSourceFile;

typedef struct CmSourceSet {
    CmSourceFile *files;
    size_t length;
    size_t capacity;
} CmSourceSet;

void cm_source_set_init(CmSourceSet *set);
void cm_source_set_destroy(CmSourceSet *set);
CmSourceStatus cm_source_add_memory(CmSourceSet *set, const char *path,
    const unsigned char *bytes, size_t length, CmSourceId *out_id);
CmSourceStatus cm_source_load_file_bounded(CmSourceSet *set,
    const char *path, size_t maximum_bytes, CmSourceId *out_id);
CmSourceStatus cm_source_load_file(CmSourceSet *set, const char *path,
    CmSourceId *out_id);
const CmSourceFile *cm_source_get(const CmSourceSet *set, CmSourceId id);
int cm_span_is_valid(const CmSourceSet *set, CmSpan span);
int cm_source_line_column(const CmSourceSet *set, CmSpan span,
    uint32_t *out_line, uint32_t *out_column);
const char *cm_source_status_name(CmSourceStatus status);

#endif
