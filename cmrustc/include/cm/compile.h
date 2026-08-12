#ifndef CMRUSTC_CM_COMPILE_H
#define CMRUSTC_CM_COMPILE_H

#include "cm/driver.h"
#include "cm/syntax/token.h"

typedef enum CmCompileStatus {
    CM_COMPILE_OK = 0,
    CM_COMPILE_INVALID_ARGUMENT,
    CM_COMPILE_SOURCE_IO,
    CM_COMPILE_MODULE_GRAPH,
    CM_COMPILE_IMPORTS,
    CM_COMPILE_HIR,
    CM_COMPILE_BODY,
    CM_COMPILE_SEMANTIC,
    CM_COMPILE_MIR,
    CM_COMPILE_CODEGEN,
    CM_COMPILE_METADATA,
    CM_COMPILE_OUTPUT_IO
} CmCompileStatus;

typedef struct CmCompileResult {
    CmCompileStatus status;
    char message[256];
} CmCompileResult;

enum CmCompileCmhirKind {
    CM_COMPILE_CMHIR_DECLARATION = 0,
    CM_COMPILE_CMHIR_SEMANTIC
};

typedef struct CmCompileCmhirDependency {
    const char *extern_name;
    const char *path;
    /* Selects the exact accepted wire version; never auto-detected. */
    enum CmCompileCmhirKind kind;
} CmCompileCmhirDependency;

/*
 * Compile one source crate to a portable C translation unit.  This function
 * never invokes a C compiler.  It completes every semantic phase and builds
 * the complete output in memory before creating a same-directory temporary,
 * then publishes it atomically.  A rejected program cannot create or truncate
 * the requested artifact, and an output path alias cannot overwrite the input.
 */
CmCompileResult cm_compile_emit_c(const char *input_path,
    const char *output_path, enum cm_edition edition,
    const CmTargetDesc *target);

/*
 * Lower one declaration crate through the ordinary source/module/import/HIR
 * pipeline and publish deterministic cmhir-meta-v1 bytes atomically.  This
 * is an explicitly cmrustc-private declaration format, not Rust `.rmeta` or
 * `.rlib`.  Dependency artifacts are decoded into the fresh HIR context
 * before the source crate is lowered, so exact external type paths and
 * imports use remapped runtime DefIds.  The current wire remains
 * self-contained: a successfully emitted artifact cannot retain references
 * to a dependency crate.
 *
 * Unsupported HIR (including traits, impls, functions/bodies, consts,
 * statics, projections, and dependency-backed named types) rejects before
 * output publication.  On every failure the existing output is unchanged.
 */
CmCompileResult cm_compile_emit_cmhir(const char *input_path,
    const char *output_path, const char *crate_name,
    enum cm_edition edition, const CmTargetDesc *target,
    const CmCompileCmhirDependency *dependencies,
    size_t dependency_count);

/*
 * Explicit variant of cm_compile_emit_cmhir. Semantic mode publishes exact
 * cmhir-meta-v1.1 and accepts only dependencies whose per-entry kind also
 * selects v1.1. Its transported trait universe remains OPEN.
 */
CmCompileResult cm_compile_emit_cmhir_kind(const char *input_path,
    const char *output_path, const char *crate_name,
    enum cm_edition edition, const CmTargetDesc *target,
    const CmCompileCmhirDependency *dependencies,
    size_t dependency_count, enum CmCompileCmhirKind output_kind);

const char *cm_compile_status_name(CmCompileStatus status);

#endif
