#ifndef CMRUSTC_CM_RESOLVE_BODY_EXPAND_H
#define CMRUSTC_CM_RESOLVE_BODY_EXPAND_H
#include "cm/macro.h"
#include "cm/macro/expand.h"
#include "cm/resolve/imports.h"
#include "cm/resolve/dependency_macro.h"
#include "cm/resolve/module_graph.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Expression-position macro expansion over every cfg-active body of a built
 * module graph (M9-01).  This is the lenient, mrustc-style pass: it assumes
 * the crate is valid Rust, resolves each invocation through the module's
 * textual macro scope or an explicit `crate::` path, expands `macro_rules!`
 * and rule-bodied `macro` definitions through the existing transcriber, and
 * splices the reparsed expression over the invocation node in place.  A
 * small set of compiler builtins (`assert`, `panic`, `unreachable`,
 * `format_args`, `const_format_args`, `cfg`, `stringify`, `concat`, `line`,
 * `column`, `file`, `module_path`, `env`, `option_env`) is implemented by
 * generating Rust source and reparsing it.  Inline assembly and the few
 * remaining builtins (`cfg_select`, `offset_of`, `include*`) are left in
 * place and counted so later stages can lower or reject them.
 */

#define CM_BODY_EXPAND_DEFAULT_MAX_DEPTH 64u
#define CM_BODY_EXPAND_FAILURE_CLASSES 16u

typedef struct CmBodyExpandOptions {
    enum cm_edition edition;
    /* Text substituted for `$crate` and used to qualify builtin paths:
     * "crate" while compiling `core`, otherwise "::core". */
    const char *crate_identifier;
    const CmCfgSet *cfg;
    /* Optional: resolved `use` imports supply path-scoped macro bindings. */
    const CmImportResolver *imports;
    /*
     * Optional dependency-crate macro artifacts (M9-03): unqualified and
     * dependency-qualified body macros resolve through these after every
     * local scope fails.  Expansions of a dependency macro substitute
     * `$crate` with that artifact's extern name.
     */
    const struct CmDependencyMacroArtifact *const *dependency_macros;
    size_t dependency_macro_count;
    unsigned int maximum_depth;
} CmBodyExpandOptions;

typedef struct CmBodyExpandResult {
    size_t bodies;
    size_t invocations;
    size_t expanded_rules;
    size_t expanded_builtin;
    /* Deliberately retained invocations: inline assembly and `offset_of!`,
     * which HIR lowering turns into dedicated nodes. */
    size_t remaining_asm;
    /* Builtins this pass does not implement yet. */
    size_t remaining_builtin;
    /* Invocations that could not be resolved or whose output did not parse. */
    size_t failed;
    char first_failure_macro[64];
    char first_failure_reason[128];
    CmSpan first_failure_span;
    /* Distinct (macro, reason) failure classes with counts, for the probe. */
    struct {
        char macro_name[64];
        char reason[128];
        size_t count;
        /* First occurrence: the module whose body failed and the span
         * start within its unit (the probe prints the source path). */
        CmModuleId module;
        uint32_t start;
    } failure_classes[CM_BODY_EXPAND_FAILURE_CLASSES];
    size_t failure_class_count;
} CmBodyExpandResult;

void cm_body_expand_options_init(CmBodyExpandOptions *options);

CmBodyExpandResult cm_body_expand_graph(CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmBodyExpandOptions *options);

#endif
