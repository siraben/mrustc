#ifndef CMRUSTC_CM_CODEGEN_C_H
#define CMRUSTC_CM_CODEGEN_C_H

#include "cm/buf.h"
#include "cm/driver.h"
#include "cm/hir/model.h"
#include "cm/mir/model.h"

typedef enum CmCEmitStatus {
    CM_C_EMIT_OK = 0,
    CM_C_EMIT_INVALID_ARGUMENT,
    CM_C_EMIT_UNSUPPORTED_TARGET,
    CM_C_EMIT_INVALID_ENTRY,
    CM_C_EMIT_UNSUPPORTED_ENTRY,
    CM_C_EMIT_INVALID_MIR
} CmCEmitStatus;

/*
 * Emit the deliberately minimal executable-C boundary.
 *
 * The accepted program is one public, free, nongeneric, zero-parameter,
 * safe `extern "C"` function named `main`, with exactly one `no_mangle`
 * attribute and an i32 return type.  The crate must have exactly the
 * `feature(no_core)`, `no_core`, and `no_main` inner attributes.  Its MIR must
 * be the canonical one-block `return_local = const i32; return` form.  No
 * partial output is appended on failure.
 */
CmCEmitStatus cm_c_emit_program(CmStrBuf *output,
    const CmHirContext *hir, const CmMirBody *body,
    CmHirItemId entry_item, const CmTargetDesc *target);

/*
 * Emit every and only exact MIR instance reachable from explicit exports.
 * The current C99 boundary includes scalar i32/u32 locals and checked local
 * nongeneric named aggregates, while exported signatures and all direct calls
 * remain scalar-u32 ABI. All validation and layout planning precedes output.
 */
CmCEmitStatus cm_c_emit_reachable_program(CmStrBuf *output,
    const CmHirContext *hir, const CmMirContext *mir,
    const CmMirBodyId *roots, uint32_t root_count,
    const CmTargetDesc *target);

const char *cm_c_emit_status_name(CmCEmitStatus status);

#endif
