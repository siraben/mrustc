#ifndef CM_DIAG_H
#define CM_DIAG_H

#include <stdio.h>

#include "cm/source.h"

typedef enum CmDiagSeverity {
    CM_DIAG_NOTE = 0,
    CM_DIAG_WARNING,
    CM_DIAG_ERROR,
    CM_DIAG_FATAL
} CmDiagSeverity;

const char *cm_diag_severity_name(CmDiagSeverity severity);
void cm_diag_emit(FILE *stream, const CmSourceSet *sources,
    CmDiagSeverity severity, CmSpan span, const char *message);

#endif
