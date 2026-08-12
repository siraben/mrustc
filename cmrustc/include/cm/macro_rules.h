#ifndef CMRUSTC_CM_MACRO_RULES_H
#define CMRUSTC_CM_MACRO_RULES_H

#include "cm/macro.h"
#include "cm/syntax/token_tree.h"
#include "cm/vec.h"

#define CM_MACRO_RULES_ABSOLUTE_MAX_NESTING 32u
#define CM_MACRO_RULES_DEFAULT_MAX_NESTING 16u
#define CM_MACRO_RULES_DEFAULT_BACKTRACK_STEPS 100000u
#define CM_MACRO_RULES_DEFAULT_REPETITION_ITERATIONS 4096u

typedef uint32_t CmMacroPatternId;
typedef uint32_t CmMacroBindingId;

#define CM_MACRO_PATTERN_NONE ((CmMacroPatternId)0)
#define CM_MACRO_BINDING_NONE ((CmMacroBindingId)0)

typedef enum CmMacroFragmentKind {
    CM_MACRO_FRAGMENT_IDENT = 0,
    CM_MACRO_FRAGMENT_EXPR,
    CM_MACRO_FRAGMENT_TY,
    CM_MACRO_FRAGMENT_PAT,
    CM_MACRO_FRAGMENT_PATH,
    CM_MACRO_FRAGMENT_TT,
    CM_MACRO_FRAGMENT_ITEM,
    CM_MACRO_FRAGMENT_BLOCK,
    CM_MACRO_FRAGMENT_LITERAL,
    CM_MACRO_FRAGMENT_VIS,
    CM_MACRO_FRAGMENT_LIFETIME,
    CM_MACRO_FRAGMENT_META
} CmMacroFragmentKind;

typedef enum CmMacroPatternKind {
    CM_MACRO_PATTERN_TOKEN = 0,
    CM_MACRO_PATTERN_GROUP,
    CM_MACRO_PATTERN_METAVARIABLE,
    CM_MACRO_PATTERN_REPETITION,
    CM_MACRO_PATTERN_CRATE,
    CM_MACRO_PATTERN_CONCAT,
    CM_MACRO_PATTERN_IGNORE,
    CM_MACRO_PATTERN_INDEX,
    CM_MACRO_PATTERN_COUNT
} CmMacroPatternKind;

typedef enum CmMacroRepetitionOperator {
    CM_MACRO_REPETITION_ZERO_OR_MORE = 0,
    CM_MACRO_REPETITION_ONE_OR_MORE,
    CM_MACRO_REPETITION_ZERO_OR_ONE
} CmMacroRepetitionOperator;

typedef struct CmMacroRulesLimits {
    /* Limits are inclusive; the next descent, step, or iteration diagnoses. */
    unsigned int max_nesting;
    size_t max_backtrack_steps;
    size_t max_repetition_iterations;
} CmMacroRulesLimits;

typedef struct CmMacroTokenPattern {
    enum cm_token_kind kind;
    size_t source_start;
    size_t source_length;
} CmMacroTokenPattern;

typedef struct CmMacroPatternNode {
    CmMacroPatternId id;
    CmMacroPatternKind kind;
    CmMacroPatternId next_sibling;
    CmMacroPatternId first_child;
    CmMacroPatternId last_child;
    union {
        CmMacroTokenPattern token;
        struct {
            enum cm_tt_delimiter delimiter;
            size_t source_start;
        } group;
        struct {
            CmMacroBindingId binding;
            CmMacroFragmentKind fragment;
        } metavariable;
        struct {
            CmMacroRepetitionOperator operator_kind;
            size_t source_start;
            int has_separator;
            CmMacroTokenPattern separator;
        } repetition;
        struct {
            size_t source_start;
        } concat;
        struct {
            size_t source_start;
        } metavariable_expression;
    } data;
} CmMacroPatternNode;

typedef struct CmMacroBinding {
    CmMacroBindingId id;
    size_t name_start;
    size_t name_length;
    CmMacroFragmentKind fragment;
    unsigned int repetition_depth;
} CmMacroBinding;

typedef struct CmMacroRuleArm {
    size_t index;
    CmMacroPatternId matcher_first;
    CmMacroPatternId transcriber_first;
    CmMacroBindingId first_binding;
    size_t binding_count;
} CmMacroRuleArm;

typedef struct CmMacroRulesDefinition {
    const struct cm_token_tree *tree;
    const char *source;
    size_t source_length;
    cm_tt_id body;
    CmMacroRulesLimits limits;
    CmVec nodes;
    CmVec bindings;
    CmVec arms;
} CmMacroRulesDefinition;

typedef struct CmMacroRulesParseResult {
    CmMacroStatus status;
    size_t arm_count;
    size_t binding_count;
    CmMacroDiagnostic diagnostic;
} CmMacroRulesParseResult;

typedef struct CmMacroCapture {
    CmMacroBindingId binding;
    CmMacroFragmentKind fragment;
    unsigned int repetition_depth;
    size_t repetition_indices[CM_MACRO_RULES_ABSOLUTE_MAX_NESTING];
    cm_tt_id parent;
    cm_tt_id first_node;
    cm_tt_id last_node;
} CmMacroCapture;

typedef struct CmMacroCaptureSet {
    const struct cm_token_tree *input_tree;
    const char *input_source;
    size_t input_source_length;
    size_t arm_index;
    CmVec captures;
} CmMacroCaptureSet;

typedef struct CmMacroRulesMatchResult {
    CmMacroStatus status;
    size_t arm_index;
    size_t backtrack_steps;
    CmMacroDiagnostic diagnostic;
} CmMacroRulesMatchResult;

typedef struct CmMacroRulesTranscribeResult {
    CmMacroStatus status;
    size_t emitted_repetitions;
    CmMacroDiagnostic diagnostic;
} CmMacroRulesTranscribeResult;

void cm_macro_rules_limits_init(CmMacroRulesLimits *limits);
const char *cm_macro_fragment_kind_name(CmMacroFragmentKind kind);

void cm_macro_rules_definition_init(CmMacroRulesDefinition *definition);
void cm_macro_rules_definition_destroy(CmMacroRulesDefinition *definition);
/*
 * The parsed definition borrows tree and source through matching and
 * transcription.  expr/ty/pat/path/item are captured as bounded opaque token
 * tree sequences in this foundation; later AST integration can replace their
 * recognizers without changing binding or capture identities.
 */
CmMacroRulesParseResult cm_macro_rules_parse(
    CmMacroRulesDefinition *definition,
    const struct cm_token_tree *tree,
    const char *source,
    size_t source_length,
    cm_tt_id body,
    const CmMacroRulesLimits *limits
);

const CmMacroPatternNode *cm_macro_rules_pattern(
    const CmMacroRulesDefinition *definition, CmMacroPatternId id);
const CmMacroBinding *cm_macro_rules_binding(
    const CmMacroRulesDefinition *definition, CmMacroBindingId id);
const CmMacroRuleArm *cm_macro_rules_arm(
    const CmMacroRulesDefinition *definition, size_t index);

void cm_macro_capture_set_init(CmMacroCaptureSet *captures);
void cm_macro_capture_set_destroy(CmMacroCaptureSet *captures);
const CmMacroCapture *cm_macro_capture(
    const CmMacroCaptureSet *captures, size_t index);

CmMacroRulesMatchResult cm_macro_rules_match(
    const CmMacroRulesDefinition *definition,
    const struct cm_token_tree *input_tree,
    const char *input_source,
    size_t input_source_length,
    cm_tt_id input,
    CmMacroCaptureSet *captures
);

/* Writes deterministic, whitespace-safe Rust tokens; clears output on error. */
CmMacroRulesTranscribeResult cm_macro_rules_transcribe(
    const CmMacroRulesDefinition *definition,
    const CmMacroCaptureSet *captures,
    CmStrBuf *output
);

/*
 * As above, but expands `$crate` to one validated Rust crate identifier.
 * This is the explicit hygiene boundary for a resolver-certified external
 * macro definition; no identifier is inferred from invocation spelling.
 */
CmMacroRulesTranscribeResult cm_macro_rules_transcribe_with_crate(
    const CmMacroRulesDefinition *definition,
    const CmMacroCaptureSet *captures,
    const char *crate_identifier,
    CmStrBuf *output
);

#endif
