#ifndef CMRUSTC_CM_CODEGEN_UMIR_C_H
#define CMRUSTC_CM_CODEGEN_UMIR_C_H

#include "cm/mir/ulower.h"
#include "cm/buf.h"

/* M9-06: C emission from u-MIR, grown by census. */

#define CM_UMIR_CEMIT_CLASSES 48u

typedef struct CmUMirCEmitResult {
    size_t bodies;      /* complete u-MIR bodies examined */
    size_t emitted;     /* every statement renderable */
    size_t statements;  /* statements examined */
    size_t rendered;    /* statements renderable as C */
    struct {
        const char *reason;
        size_t count;
    } classes[CM_UMIR_CEMIT_CLASSES];
    size_t class_count;
} CmUMirCEmitResult;

CmUMirCEmitResult cm_umir_c_emit_dry(const CmUMirSet *umir,
    const CmTyckSet *tyck);

/*
 * Render one complete u-MIR body as a standalone C function into
 * `output` (appended).  Returns 1 when every statement rendered, 0 when
 * a construct fell back to a placeholder comment; the text is always
 * syntactically complete either way.
 */
int cm_umir_c_render_body(CmStrBuf *output, const CmHirContext *hir,
    const CmUMirBody *body, const CmUBody *ub, const CmTyckSet *tyck);

#endif
