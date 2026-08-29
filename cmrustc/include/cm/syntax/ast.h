#ifndef CM_SYNTAX_AST_H
#define CM_SYNTAX_AST_H

#include <stdio.h>

#include "cm/arena.h"
#include "cm/interner.h"
#include "cm/vec.h"

typedef uint32_t CmAstPathId;
typedef uint32_t CmAstTypeId;
typedef uint32_t CmAstItemId;
typedef uint32_t CmAstAttributeId;
typedef uint32_t CmAstPatternId;
typedef uint32_t CmAstExprId;
typedef uint32_t CmAstStmtId;

#define CM_AST_PATH_NONE ((CmAstPathId)0)
#define CM_AST_TYPE_NONE ((CmAstTypeId)0)
#define CM_AST_ITEM_NONE ((CmAstItemId)0)
#define CM_AST_PATTERN_NONE ((CmAstPatternId)0)
#define CM_AST_EXPR_NONE ((CmAstExprId)0)
#define CM_AST_STMT_NONE ((CmAstStmtId)0)

typedef struct CmAstSpan {
    uint32_t start;
    uint32_t end;
} CmAstSpan;

typedef struct CmAstGenericParamBound CmAstGenericParamBound;

typedef enum CmAstGenericArgKind {
    CM_AST_GENERIC_TYPE = 0,
    CM_AST_GENERIC_LIFETIME,
    CM_AST_GENERIC_CONST,
    CM_AST_GENERIC_BINDING,
    CM_AST_GENERIC_CONSTRAINT
} CmAstGenericArgKind;

typedef struct CmAstGenericArg {
    CmAstGenericArgKind kind;
    CmInternId name;
    struct CmAstGenericArg *name_arguments;
    uint32_t name_argument_count;
    CmAstTypeId type;
    CmInternId text;
    CmAstGenericParamBound *bounds;
    uint32_t bound_count;
    CmAstSpan span;
} CmAstGenericArg;

typedef struct CmAstPathSegment {
    CmInternId name;
    CmAstGenericArg *arguments;
    uint32_t argument_count;
} CmAstPathSegment;

typedef struct CmAstPath {
    int absolute;
    CmAstPathSegment *segments;
    uint32_t segment_count;
    CmAstSpan span;
} CmAstPath;

typedef enum CmAstTypeBoundModifier {
    CM_AST_TYPE_BOUND_REQUIRED = 0,
    CM_AST_TYPE_BOUND_RELAXED,
    CM_AST_TYPE_BOUND_CONDITIONALLY_CONST
} CmAstTypeBoundModifier;

typedef struct CmAstLifetimeBinder {
    CmInternId *lifetimes;
    uint32_t lifetime_count;
    CmAstSpan span;
} CmAstLifetimeBinder;

typedef struct CmAstTypeBound {
    CmAstTypeId trait_type;
    CmInternId lifetime;
    CmAstLifetimeBinder binder;
    CmAstTypeBoundModifier modifier;
    CmAstSpan span;
} CmAstTypeBound;

typedef enum CmAstDelimiter {
    CM_AST_DELIMITER_PAREN = 0,
    CM_AST_DELIMITER_BRACE,
    CM_AST_DELIMITER_BRACKET
} CmAstDelimiter;

typedef enum CmAstMacroForm {
    CM_AST_MACRO_INVOCATION = 0,
    CM_AST_MACRO_RULES_DEFINITION,
    CM_AST_MACRO_DECLARATIVE_DEFINITION
} CmAstMacroForm;

typedef struct CmAstMacroInvocation {
    CmAstPathId path;
    CmAstMacroForm form;
    CmInternId parameters;
    CmInternId arguments;
    CmAstDelimiter delimiter;
    int has_semicolon;
} CmAstMacroInvocation;

typedef enum CmAstTypeKind {
    CM_AST_TYPE_INFER = 0,
    CM_AST_TYPE_NEVER,
    CM_AST_TYPE_PATH,
    CM_AST_TYPE_REFERENCE,
    CM_AST_TYPE_POINTER,
    CM_AST_TYPE_TUPLE,
    CM_AST_TYPE_SLICE,
    CM_AST_TYPE_ARRAY,
    CM_AST_TYPE_FUNCTION,
    CM_AST_TYPE_IMPL_TRAIT,
    CM_AST_TYPE_DYN_TRAIT,
    CM_AST_TYPE_OTHER,
    CM_AST_TYPE_PROJECTION,
    CM_AST_TYPE_MACRO
} CmAstTypeKind;

typedef enum CmAstTupleProvenance {
    /* Zero initialization must remain the restrictive source form. */
    CM_AST_TUPLE_SOURCE = 0,
    CM_AST_TUPLE_CALLABLE_INPUTS
} CmAstTupleProvenance;

typedef struct CmAstProjectionType {
    CmAstTypeId self_type;
    CmAstPathId trait_path;
    CmAstPathSegment associated;
} CmAstProjectionType;

typedef struct CmAstType {
    CmAstTypeKind kind;
    CmAstTupleProvenance tuple_provenance;
    CmAstSpan span;
    CmAstPathId path;
    CmAstTypeId child;
    CmAstTypeId *elements;
    uint32_t element_count;
    CmAstTypeBound *bounds;
    uint32_t bound_count;
    /* Present only on CM_AST_TYPE_FUNCTION. */
    CmAstLifetimeBinder binder;
    CmInternId lifetime;
    CmInternId text;
    int is_mutable;
    int is_unsafe;
    CmAstProjectionType projection;
    CmAstMacroInvocation macro_type;
} CmAstType;

typedef enum CmAstVisibilityKind {
    CM_AST_VIS_INHERITED = 0,
    CM_AST_VIS_PUBLIC,
    CM_AST_VIS_CRATE,
    CM_AST_VIS_SELF,
    CM_AST_VIS_SUPER,
    CM_AST_VIS_RESTRICTED
} CmAstVisibilityKind;

typedef struct CmAstVisibility {
    CmAstVisibilityKind kind;
    CmAstPathId restriction;
} CmAstVisibility;

typedef enum CmAstAttributeStyle {
    CM_AST_ATTR_OUTER = 0,
    CM_AST_ATTR_INNER
} CmAstAttributeStyle;

typedef struct CmAstAttribute {
    CmAstAttributeStyle style;
    CmInternId text;
    CmAstSpan span;
} CmAstAttribute;

typedef enum CmAstGenericParamKind {
    CM_AST_PARAM_TYPE = 0,
    CM_AST_PARAM_LIFETIME,
    CM_AST_PARAM_CONST
} CmAstGenericParamKind;

typedef enum CmAstGenericParamBoundModifier {
    CM_AST_GENERIC_BOUND_REQUIRED = 0,
    CM_AST_GENERIC_BOUND_RELAXED,
    CM_AST_GENERIC_BOUND_CONDITIONALLY_CONST
} CmAstGenericParamBoundModifier;

typedef enum CmAstGenericParamBoundKind {
    CM_AST_GENERIC_BOUND_TRAIT = 0,
    CM_AST_GENERIC_BOUND_LIFETIME
} CmAstGenericParamBoundKind;

struct CmAstGenericParamBound {
    CmAstGenericParamBoundKind kind;
    CmAstGenericParamBoundModifier modifier;
    CmAstTypeId trait_type;
    CmInternId lifetime;
    CmAstSpan span;
};

typedef struct CmAstGenericParam {
    CmAstAttributeId *attributes;
    uint32_t attribute_count;
    CmAstGenericParamKind kind;
    CmInternId name;
    CmInternId declaration;
    /* Exact text after `:`, retained for constraint provenance. */
    CmInternId constraint;
    /* Ordered trait/lifetime bounds for type and lifetime parameters. */
    CmAstGenericParamBound *bounds;
    uint32_t bound_count;
    /* Required declared type for a const parameter; none for other kinds. */
    CmAstTypeId declared_type;
    /* Exact const-default expression text retained as source provenance. */
    CmInternId default_const;
    /* Structural const-default expression, or none. */
    CmAstExprId default_const_expr;
    /* Structural type-parameter default, or none. */
    CmAstTypeId default_type;
} CmAstGenericParam;

typedef enum CmAstPatternKind {
    CM_AST_PATTERN_WILDCARD = 0,
    CM_AST_PATTERN_BINDING,
    CM_AST_PATTERN_LITERAL,
    CM_AST_PATTERN_PATH,
    CM_AST_PATTERN_REFERENCE,
    CM_AST_PATTERN_TUPLE,
    CM_AST_PATTERN_STRUCT,
    CM_AST_PATTERN_SLICE,
    CM_AST_PATTERN_OR,
    CM_AST_PATTERN_RANGE,
    CM_AST_PATTERN_REST
} CmAstPatternKind;

typedef struct CmAstPatternField {
    CmInternId name;
    CmAstPatternId pattern;
    int is_shorthand;
} CmAstPatternField;

typedef struct CmAstPattern {
    CmAstPatternKind kind;
    CmAstSpan span;
    union {
        struct {
            CmInternId name;
            CmAstPatternId subpattern;
            int is_ref;
            int is_mutable;
        } binding;
        struct {
            CmInternId text;
        } literal;
        struct {
            CmAstPathId path;
        } path;
        struct {
            CmAstPatternId pattern;
            int is_mutable;
        } reference;
        struct {
            CmAstPatternId *patterns;
            uint32_t pattern_count;
            int has_rest;
            uint32_t rest_index;
        } list;
        struct {
            CmAstPathId path;
            CmAstPatternField *fields;
            uint32_t field_count;
            int has_rest;
            int is_tuple;
        } struct_pattern;
        struct {
            CmAstPatternId start;
            CmAstPatternId end;
            int is_inclusive;
        } range;
    } data;
} CmAstPattern;

typedef struct CmAstExprField {
    CmInternId name;
    CmAstExprId value;
    CmAstAttributeId *attributes;
    uint32_t attribute_count;
    int is_shorthand;
    CmAstSpan span;
} CmAstExprField;

typedef struct CmAstMatchArm {
    CmAstAttributeId *attributes;
    uint32_t attribute_count;
    CmAstPatternId pattern;
    CmAstExprId guard;
    CmAstPatternId guard_pattern;
    CmAstExprId guard_initializer;
    CmAstSpan guard_span;
    CmAstExprId body;
} CmAstMatchArm;

typedef struct CmAstClosureParam {
    CmAstPatternId pattern;
    CmAstTypeId type;
} CmAstClosureParam;

typedef enum CmAstExprKind {
    CM_AST_EXPR_LITERAL = 0,
    CM_AST_EXPR_PATH,
    CM_AST_EXPR_QUALIFIED_PATH,
    CM_AST_EXPR_BLOCK,
    CM_AST_EXPR_CALL,
    CM_AST_EXPR_METHOD_CALL,
    CM_AST_EXPR_FIELD,
    CM_AST_EXPR_TUPLE_FIELD,
    CM_AST_EXPR_INDEX,
    CM_AST_EXPR_UNARY,
    CM_AST_EXPR_BINARY,
    CM_AST_EXPR_ASSIGN,
    CM_AST_EXPR_CAST,
    CM_AST_EXPR_TRY,
    CM_AST_EXPR_TRY_BLOCK,
    CM_AST_EXPR_RANGE,
    CM_AST_EXPR_LET,
    CM_AST_EXPR_RETURN,
    CM_AST_EXPR_BREAK,
    CM_AST_EXPR_CONTINUE,
    CM_AST_EXPR_IF,
    CM_AST_EXPR_MATCH,
    CM_AST_EXPR_LOOP,
    CM_AST_EXPR_WHILE,
    CM_AST_EXPR_FOR,
    CM_AST_EXPR_CLOSURE,
    CM_AST_EXPR_TUPLE,
    CM_AST_EXPR_ARRAY,
    CM_AST_EXPR_STRUCT,
    CM_AST_EXPR_MACRO,
    CM_AST_EXPR_RAW_REFERENCE
} CmAstExprKind;

typedef enum CmAstRawReferenceKind {
    CM_AST_RAW_REFERENCE_CONST = 0,
    CM_AST_RAW_REFERENCE_MUT
} CmAstRawReferenceKind;

typedef struct CmAstExpr {
    CmAstExprKind kind;
    CmAstSpan span;
    CmAstAttributeId *attributes;
    uint32_t attribute_count;
    union {
        struct { CmInternId text; } literal;
        struct { CmAstPathId path; } path;
        struct {
            CmAstTypeId self_type;
            CmAstPathId trait_path;
            CmAstPathId associated_path;
            CmAstSpan qualifier_span;
        } qualified_path;
        struct {
            CmAstAttributeId *inner_attributes;
            uint32_t inner_attribute_count;
            CmAstStmtId *statements;
            uint32_t statement_count;
            CmAstExprId tail;
            int is_unsafe;
            int is_const;
        } block;
        struct {
            CmAstExprId callee;
            CmAstExprId *arguments;
            uint32_t argument_count;
        } call;
        struct {
            CmAstExprId receiver;
            CmInternId name;
            CmAstGenericArg *generic_arguments;
            uint32_t generic_argument_count;
            CmAstSpan generic_argument_span;
            CmAstExprId *arguments;
            uint32_t argument_count;
        } method_call;
        struct {
            CmAstExprId base;
            CmInternId name;
            CmAstSpan name_span;
        } field;
        struct {
            CmAstExprId base;
            uint32_t index;
            CmAstSpan index_span;
        } tuple_field;
        struct { CmAstExprId base; CmAstExprId index; } index;
        struct { CmInternId operator_name; CmAstExprId operand; } unary;
        struct {
            CmAstRawReferenceKind kind;
            CmAstExprId operand;
        } raw_reference;
        struct {
            CmInternId operator_name;
            CmAstExprId left;
            CmAstExprId right;
        } binary;
        struct { CmAstExprId value; CmAstTypeId type; } cast;
        struct { CmAstExprId operand; } try_expr;
        struct {
            CmAstExprId start;
            CmAstExprId end;
            int is_inclusive;
        } range;
        struct {
            CmAstPatternId pattern;
            CmAstExprId initializer;
        } let_expr;
        struct { CmInternId label; CmAstExprId value; } flow;
        struct {
            CmAstExprId condition;
            CmAstPatternId pattern;
            CmAstExprId then_expr;
            CmAstExprId else_expr;
        } if_expr;
        struct {
            CmAstExprId scrutinee;
            CmAstMatchArm *arms;
            uint32_t arm_count;
        } match_expr;
        struct { CmInternId label; CmAstExprId body; } loop_expr;
        struct {
            CmAstExprId condition;
            CmAstPatternId pattern;
            CmAstExprId body;
        } while_expr;
        struct {
            CmAstPatternId pattern;
            CmAstExprId iterable;
            CmAstExprId body;
        } for_expr;
        struct {
            CmAstClosureParam *parameters;
            uint32_t parameter_count;
            CmAstTypeId return_type;
            CmAstExprId body;
            int is_move;
        } closure;
        struct {
            CmAstExprId *elements;
            uint32_t element_count;
            CmAstExprId repeat_value;
            CmAstExprId repeat_length;
        } list;
        struct {
            CmAstPathId path;
            CmAstExprField *fields;
            uint32_t field_count;
            CmAstExprId base;
        } struct_expr;
        CmAstMacroInvocation macro_expr;
    } data;
} CmAstExpr;

typedef enum CmAstStmtKind {
    CM_AST_STMT_LET = 0,
    CM_AST_STMT_EXPR,
    CM_AST_STMT_ITEM
} CmAstStmtKind;

typedef struct CmAstStmt {
    CmAstStmtKind kind;
    CmAstSpan span;
    CmAstAttributeId *attributes;
    uint32_t attribute_count;
    union {
        struct {
            CmAstPatternId pattern;
            CmAstTypeId type;
            CmAstExprId initializer;
            CmAstExprId else_block;
        } let_stmt;
        struct {
            CmAstExprId expression;
            int has_semicolon;
        } expr_stmt;
        struct {
            CmAstItemId item;
        } item_stmt;
    } data;
} CmAstStmt;

typedef struct CmAstFunctionParam {
    CmAstPatternId pattern;
    CmAstTypeId type;
    CmInternId receiver_lifetime;
    int is_self;
} CmAstFunctionParam;

typedef enum CmAstFieldForm {
    CM_AST_FIELDS_UNIT = 0,
    CM_AST_FIELDS_TUPLE,
    CM_AST_FIELDS_NAMED
} CmAstFieldForm;

typedef struct CmAstField {
    CmInternId name;
    CmAstVisibility visibility;
    CmAstTypeId type;
    /* Outer attributes (`#[cfg(..)] pub tv_usec: T` in libc): the module
     * graph drops cfg-inactive fields in place. */
    CmAstAttributeId *attributes;
    uint32_t attribute_count;
} CmAstField;

typedef struct CmAstVariant {
    CmInternId name;
    CmAstAttributeId *attributes;
    uint32_t attribute_count;
    CmAstFieldForm form;
    CmAstField *fields;
    uint32_t field_count;
    CmInternId discriminant;
    CmAstSpan span;
} CmAstVariant;

typedef enum CmAstItemKind {
    CM_AST_ITEM_FUNCTION = 0,
    CM_AST_ITEM_STRUCT,
    CM_AST_ITEM_ENUM,
    CM_AST_ITEM_TYPE_ALIAS,
    CM_AST_ITEM_CONST,
    CM_AST_ITEM_STATIC,
    CM_AST_ITEM_MODULE,
    CM_AST_ITEM_USE,
    CM_AST_ITEM_EXTERN_CRATE,
    CM_AST_ITEM_EXTERN_BLOCK,
    CM_AST_ITEM_TRAIT,
    CM_AST_ITEM_IMPL,
    CM_AST_ITEM_MACRO,
    CM_AST_ITEM_UNION
} CmAstItemKind;

typedef struct CmAstFunction {
    CmAstFunctionParam *parameters;
    uint32_t parameter_count;
    CmAstTypeId return_type;
    CmInternId abi;
    CmAstExprId body;
    int is_const;
    int is_async;
    int is_variadic;    /* `fn f(a: T, ...)` (foreign declarations) */
    int is_safe;
    int is_unsafe;
} CmAstFunction;

typedef struct CmAstAggregate {
    CmAstFieldForm form;
    CmAstField *fields;
    uint32_t field_count;
} CmAstAggregate;

typedef struct CmAstEnum {
    CmAstVariant *variants;
    uint32_t variant_count;
} CmAstEnum;

typedef struct CmAstModule {
    CmAstAttributeId *inner_attributes;
    uint32_t inner_attribute_count;
    CmAstItemId *items;
    uint32_t item_count;
    int is_inline;
} CmAstModule;

typedef enum CmAstSupertraitModifier {
    CM_AST_SUPERTRAIT_REQUIRED = 0,
    CM_AST_SUPERTRAIT_CONDITIONALLY_CONST
} CmAstSupertraitModifier;

typedef enum CmAstSupertraitKind {
    CM_AST_SUPERTRAIT_TRAIT = 0,
    CM_AST_SUPERTRAIT_LIFETIME
} CmAstSupertraitKind;

typedef struct CmAstSupertrait {
    CmAstSupertraitKind kind;
    CmAstSupertraitModifier modifier;
    CmAstTypeId type;
    CmInternId lifetime;
    CmAstSpan span;
} CmAstSupertrait;

typedef enum CmAstAssociatedTypeBoundModifier {
    CM_AST_ASSOC_BOUND_REQUIRED = 0,
    CM_AST_ASSOC_BOUND_RELAXED
} CmAstAssociatedTypeBoundModifier;

typedef enum CmAstAssociatedTypeBoundKind {
    CM_AST_ASSOC_BOUND_TRAIT = 0,
    CM_AST_ASSOC_BOUND_LIFETIME
} CmAstAssociatedTypeBoundKind;

typedef struct CmAstAssociatedTypeBound {
    CmAstAssociatedTypeBoundKind kind;
    CmAstAssociatedTypeBoundModifier modifier;
    CmAstTypeId trait_type;
    CmInternId lifetime;
    CmAstSpan span;
} CmAstAssociatedTypeBound;

typedef enum CmAstWhereBoundModifier {
    CM_AST_WHERE_BOUND_REQUIRED = 0,
    CM_AST_WHERE_BOUND_RELAXED,
    CM_AST_WHERE_BOUND_CONDITIONALLY_CONST,
    CM_AST_WHERE_BOUND_CONST
} CmAstWhereBoundModifier;

typedef enum CmAstWhereBoundKind {
    CM_AST_WHERE_BOUND_TRAIT = 0,
    CM_AST_WHERE_BOUND_LIFETIME
} CmAstWhereBoundKind;

typedef struct CmAstWhereBound {
    CmAstWhereBoundKind kind;
    CmAstWhereBoundModifier modifier;
    CmAstTypeId trait_type;
    CmInternId lifetime;
    CmAstLifetimeBinder binder;
    CmAstSpan span;
} CmAstWhereBound;

typedef enum CmAstWherePredicateKind {
    CM_AST_WHERE_PREDICATE_TYPE = 0,
    CM_AST_WHERE_PREDICATE_LIFETIME
} CmAstWherePredicateKind;

typedef struct CmAstWherePredicate {
    CmAstWherePredicateKind kind;
    CmAstTypeId subject;
    CmInternId subject_lifetime;
    CmAstLifetimeBinder binder;
    CmAstWhereBound *bounds;
    uint32_t bound_count;
    CmAstSpan span;
} CmAstWherePredicate;

typedef struct CmAstTrait {
    CmInternId supertraits;
    CmAstSupertrait *structured_supertraits;
    uint32_t structured_supertrait_count;
    CmInternId alias_bounds;
    CmAstSupertrait *structured_alias_bounds;
    uint32_t structured_alias_bound_count;
    CmAstItemId *items;
    uint32_t item_count;
    int is_unsafe;
    int is_alias;
    int is_auto;
} CmAstTrait;

typedef struct CmAstImpl {
    CmAstTypeId trait_type;
    CmAstTypeId self_type;
    CmAstItemId *items;
    uint32_t item_count;
    int is_unsafe;
    int is_const;
    int is_negative;
} CmAstImpl;

typedef struct CmAstItem {
    CmAstItemKind kind;
    CmAstSpan span;
    CmInternId name;
    CmAstVisibility visibility;
    CmAstAttributeId *attributes;
    uint32_t attribute_count;
    CmAstGenericParam *generic_parameters;
    uint32_t generic_parameter_count;
    /* Exact source text after `where`, retained for provenance. */
    CmInternId where_clause;
    /* Ordered structural type-bound predicates parsed from that clause. */
    CmAstWherePredicate *where_predicates;
    uint32_t where_predicate_count;
    /* Specialization qualifier on an impl or one of its associated items. */
    int is_default;
    union {
        CmAstFunction function_item;
        CmAstAggregate aggregate_item;
        CmAstEnum enum_item;
        struct {
            CmAstTypeId type;
            CmAstExprId initializer;
            CmAstAssociatedTypeBound *bounds;
            uint32_t bound_count;
            /* Kept distinct because Rust permits clauses on both sides of
               `=` in type aliases. */
            CmInternId post_value_where_clause;
            CmAstWherePredicate *post_value_where_predicates;
            uint32_t post_value_where_predicate_count;
            int has_value;
            int is_mutable;
        } value_item;
        CmAstModule module_item;
        struct {
            CmInternId tree;
        } use_item;
        struct {
            CmInternId alias;
        } extern_crate_item;
        struct {
            CmInternId abi;
            CmAstItemId *items;
            uint32_t item_count;
            int is_unsafe;
        } extern_block_item;
        CmAstTrait trait_item;
        CmAstImpl impl_item;
        CmAstMacroInvocation macro_item;
    } data;
} CmAstItem;

typedef struct CmAst {
    CmArena storage;
    CmInterner strings;
    CmVec paths;
    CmVec types;
    CmVec attributes;
    CmVec patterns;
    CmVec expressions;
    CmVec statements;
    CmVec items;
    CmVec crate_attributes;
    CmVec root_items;
} CmAst;

void cm_ast_init(CmAst *ast);
void cm_ast_destroy(CmAst *ast);
CmAstPathId cm_ast_add_path(CmAst *ast, const CmAstPath *path);
CmAstTypeId cm_ast_add_type(CmAst *ast, const CmAstType *type);
CmAstAttributeId cm_ast_add_attribute(CmAst *ast,
    const CmAstAttribute *attribute);
CmAstPatternId cm_ast_add_pattern(CmAst *ast, const CmAstPattern *pattern);
CmAstExprId cm_ast_add_expr(CmAst *ast, const CmAstExpr *expression);
CmAstStmtId cm_ast_add_stmt(CmAst *ast, const CmAstStmt *statement);
CmAstItemId cm_ast_add_item(CmAst *ast, const CmAstItem *item);
const CmAstPath *cm_ast_get_path(const CmAst *ast, CmAstPathId id);
const CmAstType *cm_ast_get_type(const CmAst *ast, CmAstTypeId id);
const CmAstAttribute *cm_ast_get_attribute(const CmAst *ast,
    CmAstAttributeId id);
const CmAstPattern *cm_ast_get_pattern(const CmAst *ast, CmAstPatternId id);
const CmAstExpr *cm_ast_get_expr(const CmAst *ast, CmAstExprId id);
const CmAstStmt *cm_ast_get_stmt(const CmAst *ast, CmAstStmtId id);
const CmAstItem *cm_ast_get_item(const CmAst *ast, CmAstItemId id);
const CmInternedString *cm_ast_get_string(const CmAst *ast, CmInternId id);

/* Stable, whitespace-insensitive-to-source diagnostic form for tests. */
int cm_ast_dump(FILE *stream, const CmAst *ast);

#endif
