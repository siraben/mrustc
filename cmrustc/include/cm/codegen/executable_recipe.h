#ifndef CMRUSTC_CM_CODEGEN_EXECUTABLE_RECIPE_H
#define CMRUSTC_CM_CODEGEN_EXECUTABLE_RECIPE_H

#include "cm/buf.h"
#include "cm/driver.h"
#include "cm/hir/admission.h"

typedef enum CmExecutableRecipeEmitStatus {
    CM_EXECUTABLE_RECIPE_EMIT_OK = 0,
    CM_EXECUTABLE_RECIPE_EMIT_INVALID_ARGUMENT,
    CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_TARGET,
    CM_EXECUTABLE_RECIPE_EMIT_INVALID_AUTHORITY,
    CM_EXECUTABLE_RECIPE_EMIT_UNSUPPORTED_PROGRAM,
    CM_EXECUTABLE_RECIPE_EMIT_INVALID_HIR
} CmExecutableRecipeEmitStatus;

/*
 * Emit the exact cmhir-meta-v3.2 RETURN_ARGUMENT consumer slice.
 *
 * The authority must be a current whole-local REGIONS admission for
 * `local_crate`. Every local item must be a public, nongeneric, safe
 * `#[no_mangle] extern "C"` wrapper with one concrete primitive parameter.
 * Its admitted typed body must directly call a foreign one-type-parameter
 * function whose authenticated metadata recipe returns its sole argument.
 * The transported marker-trait predicate and its exact positive primitive
 * impl are rechecked structurally in addition to consuming the admitted
 * direct-call recipe. Failure never appends partial output.
 */
CmExecutableRecipeEmitStatus cm_c_emit_executable_recipe_program(
    const CmHirContext *hir, const CmSemanticAdmission *authority,
    CmHirCrateId local_crate, const CmTargetDesc *target,
    CmStrBuf *output);

const char *cm_executable_recipe_emit_status_name(
    CmExecutableRecipeEmitStatus status);

#endif
