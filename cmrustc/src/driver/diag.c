#include "cm/diag.h"

const char *cm_diag_severity_name(CmDiagSeverity severity)
{
    switch (severity) {
    case CM_DIAG_NOTE:
        return "note";
    case CM_DIAG_WARNING:
        return "warning";
    case CM_DIAG_ERROR:
        return "error";
    case CM_DIAG_FATAL:
        return "fatal";
    }
    return "diagnostic";
}

void cm_diag_emit(FILE *stream, const CmSourceSet *sources,
    CmDiagSeverity severity, CmSpan span, const char *message)
{
    const CmSourceFile *file;
    uint32_t line;
    uint32_t column;

    if (stream == NULL) {
        return;
    }
    if (message == NULL) {
        message = "";
    }
    file = cm_source_get(sources, span.source);
    if (file != NULL &&
        cm_source_line_column(sources, span, &line, &column)) {
        fprintf(stream, "%s:%lu:%lu: %s: %s\n", file->path,
            (unsigned long)line, (unsigned long)column,
            cm_diag_severity_name(severity), message);
    } else {
        fprintf(stream, "<unknown>: %s: %s\n",
            cm_diag_severity_name(severity), message);
    }
}
