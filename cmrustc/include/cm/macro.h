#ifndef CMRUSTC_CM_MACRO_H
#define CMRUSTC_CM_MACRO_H

#include "cm/buf.h"
#include "cm/syntax/token.h"

typedef enum CmMacroStatus {
    CM_MACRO_OK = 0,
    CM_MACRO_INVALID_ARGUMENT,
    CM_MACRO_SYNTAX_ERROR,
    CM_MACRO_UNSUPPORTED,
    CM_MACRO_ENV_NOT_FOUND,
    CM_MACRO_NO_MATCH,
    CM_MACRO_LIMIT_EXCEEDED
} CmMacroStatus;

typedef enum CmMacroDiagnosticCode {
    CM_MACRO_DIAG_NONE = 0,
    CM_MACRO_DIAG_INVALID_ARGUMENT,
    CM_MACRO_DIAG_CFG_EXPECTED_PREDICATE,
    CM_MACRO_DIAG_CFG_EXPECTED_STRING,
    CM_MACRO_DIAG_CFG_INVALID_ESCAPE,
    CM_MACRO_DIAG_CFG_EXPECTED_COMMA_OR_CLOSE,
    CM_MACRO_DIAG_CFG_UNKNOWN_OPERATOR,
    CM_MACRO_DIAG_CFG_NOT_ARITY,
    CM_MACRO_DIAG_CFG_NESTING_LIMIT,
    CM_MACRO_DIAG_CFG_TRAILING_INPUT,
    CM_MACRO_DIAG_BUILTIN_UNKNOWN,
    CM_MACRO_DIAG_BUILTIN_EXPECTED_EMPTY,
    CM_MACRO_DIAG_BUILTIN_EXPECTED_STRING,
    CM_MACRO_DIAG_BUILTIN_EXPECTED_LITERAL,
    CM_MACRO_DIAG_BUILTIN_EXPECTED_COMMA,
    CM_MACRO_DIAG_BUILTIN_ENV_MISSING,
    CM_MACRO_DIAG_BUILTIN_INVALID_CONTEXT,
    CM_MACRO_DIAG_RULES_INVALID_TREE,
    CM_MACRO_DIAG_RULES_EXPECTED_MATCHER,
    CM_MACRO_DIAG_RULES_EXPECTED_ARROW,
    CM_MACRO_DIAG_RULES_EXPECTED_TRANSCRIBER,
    CM_MACRO_DIAG_RULES_EXPECTED_FRAGMENT,
    CM_MACRO_DIAG_RULES_UNKNOWN_FRAGMENT,
    CM_MACRO_DIAG_RULES_EXPECTED_REPEAT_OPERATOR,
    CM_MACRO_DIAG_RULES_INVALID_SEPARATOR,
    CM_MACRO_DIAG_RULES_DUPLICATE_BINDING,
    CM_MACRO_DIAG_RULES_UNKNOWN_BINDING,
    CM_MACRO_DIAG_RULES_NESTING_LIMIT,
    CM_MACRO_DIAG_RULES_BACKTRACK_LIMIT,
    CM_MACRO_DIAG_RULES_REPETITION_LIMIT,
    CM_MACRO_DIAG_RULES_EMPTY_REPETITION,
    CM_MACRO_DIAG_RULES_NO_MATCH,
    CM_MACRO_DIAG_RULES_TRANSCRIBE_REPETITION
} CmMacroDiagnosticCode;

typedef struct CmMacroDiagnostic {
    CmMacroDiagnosticCode code;
    size_t offset;
    const char *message;
} CmMacroDiagnostic;

const char *cm_macro_status_name(CmMacroStatus status);
const char *cm_macro_diagnostic_code_name(CmMacroDiagnosticCode code);

/* A NULL value represents a word cfg such as `test`. */
typedef struct CmCfgEntry {
    const char *name;
    const char *value;
} CmCfgEntry;

typedef struct CmCfgEnvironment {
    const char *target_arch;
    const char *target_os;
    const char *target_env;
    const char *target_abi;
    const char *target_vendor;
    const char *target_family;
    const char *target_pointer_width;
    const char *target_endian;
    const char *const *features;
    size_t feature_count;
    const char *const *target_features;
    size_t target_feature_count;
    const CmCfgEntry *entries;
    size_t entry_count;
} CmCfgEnvironment;

typedef struct CmCfgEvaluation {
    CmMacroStatus status;
    int value;
    CmMacroDiagnostic diagnostic;
} CmCfgEvaluation;

void cm_cfg_environment_init(CmCfgEnvironment *environment);
CmCfgEvaluation cm_cfg_evaluate(const CmCfgEnvironment *environment,
    const char *predicate, size_t predicate_length);

/* The returned value says whether cfg_attr should emit its attribute payload. */
CmCfgEvaluation cm_cfg_attr_decide(const CmCfgEnvironment *environment,
    const char *predicate, size_t predicate_length);

typedef enum CmBuiltinMacroKind {
    CM_BUILTIN_MACRO_UNKNOWN = 0,
    CM_BUILTIN_MACRO_LINE,
    CM_BUILTIN_MACRO_COLUMN,
    CM_BUILTIN_MACRO_FILE,
    CM_BUILTIN_MACRO_STRINGIFY,
    CM_BUILTIN_MACRO_CONCAT,
    CM_BUILTIN_MACRO_ENV,
    CM_BUILTIN_MACRO_OPTION_ENV,
    CM_BUILTIN_MACRO_MODULE_PATH,
    CM_BUILTIN_MACRO_CFG
} CmBuiltinMacroKind;

typedef const char *(*CmMacroEnvLookup)(void *context,
    const char *name, size_t name_length, size_t *value_length);

typedef struct CmBuiltinContext {
    const char *file;
    size_t file_length;
    size_t line;
    size_t column;
    const char *module_path;
    size_t module_path_length;
    const CmCfgEnvironment *cfg;
    CmMacroEnvLookup env_lookup;
    void *env_context;
} CmBuiltinContext;

typedef struct CmMacroExpansion {
    CmMacroStatus status;
    CmBuiltinMacroKind kind;
    CmMacroDiagnostic diagnostic;
} CmMacroExpansion;

void cm_builtin_context_init(CmBuiltinContext *context);
CmBuiltinMacroKind cm_builtin_macro_classify(const char *name,
    size_t name_length);
const char *cm_builtin_macro_kind_name(CmBuiltinMacroKind kind);

/*
 * Clears output, then writes a Rust source fragment on success.  Every
 * failure returns a non-NONE diagnostic and leaves output empty.
 */
CmMacroExpansion cm_builtin_macro_expand(CmBuiltinMacroKind kind,
    const char *arguments, size_t argument_length,
    const CmBuiltinContext *context, CmStrBuf *output);

#endif
