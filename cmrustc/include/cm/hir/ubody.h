#ifndef CMRUSTC_CM_HIR_UBODY_H
#define CMRUSTC_CM_HIR_UBODY_H
#include "cm/hir/model.h"
#include "cm/hir/module_map.h"
#include "cm/interner.h"
#include "cm/resolve/imports.h"
#include "cm/resolve/module_graph.h"
#include "cm/syntax/ast.h"
#include "cm/vec.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Untyped HIR bodies (M9-02).  Every cfg-active body of a lowered crate is
 * re-expressed as a lenient, mrustc-style expression tree: one arena per
 * body holding expressions, patterns, statements, and locals.  Paths are
 * resolved as far as name resolution allows (locals, items, enum variants,
 * `Self`, generic parameters, primitives); the remainder is kept as named
 * segments for type-directed resolution during typeck.  Types written in
 * bodies are kept as AST references for the same reason.  Nothing here is
 * typed; the typed G3 body pipeline in body.c is untouched.
 */

typedef uint32_t CmUExprId;
typedef uint32_t CmUPatId;
typedef uint32_t CmUStmtId;
#define CM_U_EXPR_NONE ((CmUExprId)0)
#define CM_U_PAT_NONE ((CmUPatId)0)
#define CM_U_LOCAL_NONE UINT32_MAX

/* A type written in a body, kept as a source-qualified AST reference. */
typedef struct CmUAstTypeRef {
    CmSourceId source;
    CmAstTypeId type;
} CmUAstTypeRef;

typedef struct CmUAstPathRef {
    CmSourceId source;
    CmAstPathId path;
} CmUAstPathRef;

typedef enum CmUResolutionKind {
    CM_U_RESOLVED_NONE = 0,
    CM_U_RESOLVED_LOCAL,          /* body local */
    CM_U_RESOLVED_DEFINITION,     /* item DefId (fn, const, static, ctor, ...) */
    CM_U_RESOLVED_VARIANT,        /* enum variant DefId */
    CM_U_RESOLVED_SELF_TYPE,      /* `Self` prefix; `rest_from` names the tail */
    CM_U_RESOLVED_GENERIC_PARAM,  /* generic parameter prefix */
    CM_U_RESOLVED_PRIMITIVE,      /* primitive type prefix (`u32::MAX`) */
    CM_U_RESOLVED_TYPE_ASSOC,     /* type DefId prefix + associated tail */
    CM_U_RESOLVED_NESTED_ITEM,    /* item declared inside a body (AST ref) */
    CM_U_RESOLVED_UNRESOLVED      /* nothing resolved; segments retained */
} CmUResolutionKind;

typedef struct CmUResolution {
    CmUResolutionKind kind;
    uint32_t local;
    CmHirDefId definition;
    CmHirGenericParamId generic_parameter;
    CmHirPrimitiveKind primitive;
    /* NESTED_ITEM: the declaring source unit and AST item. */
    CmSourceId nested_source;
    CmAstItemId nested_item;
    /* Index of the first segment not consumed by the resolution. */
    uint32_t rest_from;
} CmUResolution;

typedef enum CmULiteralKind {
    CM_U_LITERAL_INTEGER = 0,
    CM_U_LITERAL_FLOAT,
    CM_U_LITERAL_BOOL,
    CM_U_LITERAL_CHAR,
    CM_U_LITERAL_STRING,
    CM_U_LITERAL_BYTE,
    CM_U_LITERAL_BYTE_STRING,
    CM_U_LITERAL_C_STRING,
    CM_U_LITERAL_UNIT
} CmULiteralKind;

typedef enum CmUUnaryOp {
    CM_U_UNARY_NEG = 0,
    CM_U_UNARY_NOT,
    CM_U_UNARY_DEREF
} CmUUnaryOp;

typedef enum CmUBinaryOp {
    CM_U_BINARY_ADD = 0, CM_U_BINARY_SUB, CM_U_BINARY_MUL, CM_U_BINARY_DIV,
    CM_U_BINARY_REM, CM_U_BINARY_AND, CM_U_BINARY_OR, CM_U_BINARY_BIT_AND,
    CM_U_BINARY_BIT_OR, CM_U_BINARY_BIT_XOR, CM_U_BINARY_SHL, CM_U_BINARY_SHR,
    CM_U_BINARY_EQ, CM_U_BINARY_NE, CM_U_BINARY_LT, CM_U_BINARY_LE,
    CM_U_BINARY_GT, CM_U_BINARY_GE
} CmUBinaryOp;

typedef enum CmUExprKind {
    CM_U_EXPR_LITERAL = 0,
    CM_U_EXPR_PATH,            /* value path, resolved or retained */
    CM_U_EXPR_QUALIFIED_PATH,  /* <T as Trait>::name */
    CM_U_EXPR_BLOCK,
    CM_U_EXPR_CALL,
    CM_U_EXPR_METHOD_CALL,
    CM_U_EXPR_FIELD,
    CM_U_EXPR_TUPLE_FIELD,
    CM_U_EXPR_INDEX,
    CM_U_EXPR_UNARY,
    CM_U_EXPR_REF,             /* &, &mut, &raw const, &raw mut */
    CM_U_EXPR_BINARY,
    CM_U_EXPR_ASSIGN,
    CM_U_EXPR_ASSIGN_OP,
    CM_U_EXPR_CAST,
    CM_U_EXPR_TRY,
    CM_U_EXPR_RANGE,
    CM_U_EXPR_LET,             /* `let pat = expr` in a condition */
    CM_U_EXPR_RETURN,
    CM_U_EXPR_BREAK,
    CM_U_EXPR_CONTINUE,
    CM_U_EXPR_IF,
    CM_U_EXPR_MATCH,
    CM_U_EXPR_LOOP,
    CM_U_EXPR_WHILE,
    CM_U_EXPR_FOR,
    CM_U_EXPR_CLOSURE,
    CM_U_EXPR_TUPLE,
    CM_U_EXPR_ARRAY,
    CM_U_EXPR_ARRAY_REPEAT,
    CM_U_EXPR_STRUCT,
    CM_U_EXPR_ASM,             /* retained inline assembly text */
    CM_U_EXPR_OFFSET_OF,       /* retained `offset_of!` text */
    CM_U_EXPR_UNSUPPORTED
} CmUExprKind;

typedef struct CmUMatchArm {
    CmUPatId pattern;
    CmUExprId guard;
    CmUExprId body;
} CmUMatchArm;

typedef struct CmUField {
    CmInternId name;
    CmUExprId value;
} CmUField;

typedef struct CmUClosureParam {
    CmUPatId pattern;
    CmUAstTypeRef type;
} CmUClosureParam;

typedef struct CmUExpr {
    CmUExprKind kind;
    CmSpan span;
    /* Originating AST node (in the body's source unit) for later passes. */
    CmAstExprId ast;
    union {
        struct {
            CmULiteralKind kind;
            CmInternId text;      /* source spelling */
            uint64_t value_low;   /* integers: parsed magnitude */
            uint64_t value_high;
            CmInternId suffix;    /* `u32`, `f64`, ... or none */
        } literal;
        struct {
            CmUAstPathRef ast;
            CmUResolution resolution;
            CmInternId *segments;
            uint32_t segment_count;
        } path;
        struct {
            CmUAstTypeRef self_type;
            CmUAstPathRef trait_path;
            CmUAstPathRef associated_path;
        } qualified_path;
        struct {
            CmUStmtId *statements;
            uint32_t statement_count;
            CmUExprId tail;
            int is_unsafe;
            int is_const;
            CmInternId label;
        } block;
        struct {
            CmUExprId callee;
            CmUExprId *arguments;
            uint32_t argument_count;
        } call;
        struct {
            CmUExprId receiver;
            CmInternId name;
            CmUAstPathRef generic_arguments; /* path NONE when absent */
            CmUExprId *arguments;
            uint32_t argument_count;
        } method_call;
        struct {
            CmUExprId base;
            CmInternId name;
        } field;
        struct {
            CmUExprId base;
            uint32_t index;
        } tuple_field;
        struct {
            CmUExprId base;
            CmUExprId index;
        } index;
        struct {
            CmUUnaryOp op;
            CmUExprId operand;
        } unary;
        struct {
            CmUExprId operand;
            int is_mutable;
            int is_raw;
        } ref;
        struct {
            CmUBinaryOp op;
            CmUExprId left;
            CmUExprId right;
        } binary;
        struct {
            CmUExprId target;
            CmUExprId value;
            CmUBinaryOp op; /* ASSIGN_OP only */
        } assign;
        struct {
            CmUExprId value;
            CmUAstTypeRef type;
        } cast;
        struct {
            CmUExprId operand;
        } try_expr;
        struct {
            CmUExprId start;
            CmUExprId end;
            int is_inclusive;
        } range;
        struct {
            CmUPatId pattern;
            CmUExprId initializer;
        } let_expr;
        struct {
            CmInternId label;
            CmUExprId value;
        } flow;
        struct {
            CmUExprId condition;
            CmUPatId pattern; /* `if let`: pattern matched against condition */
            CmUExprId then_expr;
            CmUExprId else_expr;
        } if_expr;
        struct {
            CmUExprId scrutinee;
            CmUMatchArm *arms;
            uint32_t arm_count;
        } match_expr;
        struct {
            CmInternId label;
            CmUExprId body;
        } loop_expr;
        struct {
            CmInternId label;
            CmUExprId condition;
            CmUPatId pattern; /* `while let` */
            CmUExprId body;
        } while_expr;
        struct {
            CmInternId label;
            CmUPatId pattern;
            CmUExprId iterable;
            CmUExprId body;
        } for_expr;
        struct {
            CmUClosureParam *parameters;
            uint32_t parameter_count;
            CmUAstTypeRef return_type;
            CmUExprId body;
            int is_move;
        } closure;
        struct {
            CmUExprId *elements;
            uint32_t element_count;
        } list;
        struct {
            CmUExprId value;
            CmUExprId length;
        } repeat;
        struct {
            CmUAstPathRef ast;
            CmUResolution resolution;
            CmUField *fields;
            uint32_t field_count;
            CmUExprId base;
        } struct_expr;
        struct {
            CmInternId text; /* macro argument text */
        } retained;
    } data;
} CmUExpr;

typedef enum CmUPatKind {
    CM_U_PAT_WILD = 0,
    CM_U_PAT_BINDING,       /* name [@ subpattern] */
    CM_U_PAT_LITERAL,
    CM_U_PAT_PATH,          /* unit variant / const / unit struct */
    CM_U_PAT_TUPLE,
    CM_U_PAT_TUPLE_STRUCT,  /* Path(p, ..) */
    CM_U_PAT_STRUCT,        /* Path { f: p, .. } */
    CM_U_PAT_REF,
    CM_U_PAT_SLICE,
    CM_U_PAT_OR,
    CM_U_PAT_RANGE,
    CM_U_PAT_REST           /* `..` inside tuple/slice */
} CmUPatKind;

typedef struct CmUPatField {
    CmInternId name;
    CmUPatId pattern;
} CmUPatField;

typedef struct CmUPat {
    CmUPatKind kind;
    CmSpan span;
    CmAstPatternId ast;
    union {
        struct {
            uint32_t local;
            CmUPatId subpattern;
            int by_ref;
            int is_mutable;
        } binding;
        struct {
            CmInternId text;
            int negated;
        } literal;
        struct {
            CmUAstPathRef ast;
            CmUResolution resolution;
            CmInternId *segments;
            uint32_t segment_count;
        } path;
        struct {
            CmUPatId *patterns;
            uint32_t pattern_count;
            int has_rest;
            uint32_t rest_index;
        } list;
        struct {
            CmUAstPathRef ast;
            CmUResolution resolution;
            CmUPatId *patterns;      /* tuple-struct positional */
            uint32_t pattern_count;
            CmUPatField *fields;     /* struct named */
            uint32_t field_count;
            int has_rest;
            uint32_t rest_index;
        } struct_pat;
        struct {
            CmUPatId pattern;
            int is_mutable;
        } ref;
        struct {
            CmUPatId start;
            CmUPatId end;
            int is_inclusive;
        } range;
    } data;
} CmUPat;

typedef enum CmUStmtKind {
    CM_U_STMT_LET = 0,
    CM_U_STMT_EXPR,
    CM_U_STMT_ITEM
} CmUStmtKind;

typedef struct CmUStmt {
    CmUStmtKind kind;
    CmSpan span;
    union {
        struct {
            CmUPatId pattern;
            CmUAstTypeRef type;
            CmUExprId initializer;
            CmUExprId else_block;
        } let_stmt;
        struct {
            CmUExprId expression;
            int has_semicolon;
        } expr_stmt;
        struct {
            CmSourceId source;
            CmAstItemId item;
        } item_stmt;
    } data;
} CmUStmt;

typedef struct CmULocal {
    CmInternId name;
    int is_mutable;
    int by_ref;
    CmSpan span;
} CmULocal;

typedef enum CmUBodyStatus {
    CM_U_BODY_LOWERED = 0,
    CM_U_BODY_NO_SOURCE,   /* body has no AST (generated, recipe) */
    CM_U_BODY_FAILED
} CmUBodyStatus;

typedef struct CmUBody {
    CmHirBodyId hir_body;
    CmHirDefId owner;
    CmUBodyStatus status;
    const char *failure;
    CmVec expressions;  /* CmUExpr */
    CmVec patterns;     /* CmUPat */
    CmVec statements;   /* CmUStmt */
    CmVec locals;       /* CmULocal */
    CmUExprId root;
    /* Parameter patterns in declaration order (`self` is a binding). */
    CmUPatId *parameters;
    uint32_t parameter_count;
    CmUAstTypeRef *parameter_types;
    CmUAstTypeRef return_type;
    /* Census. */
    uint32_t unresolved_paths;
    uint32_t nested_items;
    uint32_t retained_macros;
} CmUBody;

typedef struct CmUBodySet {
    CmInterner strings;
    CmVec bodies; /* CmUBody, one per HIR body id (index = id - 1) */
    CmArena storage;
} CmUBodySet;

typedef struct CmUBodyLowerResult {
    size_t bodies;
    size_t lowered;
    size_t no_source;
    size_t failed;
    size_t expressions;
    size_t unresolved_paths;
    size_t nested_items;
    size_t retained_macros;
    const char *first_failure;
    CmHirBodyId first_failure_body;
} CmUBodyLowerResult;

void cm_ubody_set_init(CmUBodySet *set);
void cm_ubody_set_destroy(CmUBodySet *set);
const CmUBody *cm_ubody_get(const CmUBodySet *set, CmHirBodyId body);
const CmUExpr *cm_ubody_get_expr(const CmUBody *body, CmUExprId id);
const CmUPat *cm_ubody_get_pat(const CmUBody *body, CmUPatId id);
const CmUStmt *cm_ubody_get_stmt(const CmUBody *body, CmUStmtId id);

/*
 * Lower every UNLOWERED HIR body whose origin is a source item.  The graph
 * must be the one the HIR was lowered from (after expression-position macro
 * expansion) and `imports` its resolved import table.
 */
/* One dependency crate's graph bundle for cross-crate body lowering. */
typedef struct CmUBodyDependency {
    const CmModuleGraph *graph;
    CmModuleGraphRevision revision;
    const CmImportResolver *imports;
    const CmHirModuleMap *modules;
} CmUBodyDependency;

CmUBodyLowerResult cm_ubody_lower_all(CmUBodySet *set,
    const CmHirContext *hir, const CmModuleGraph *graph,
    CmModuleGraphRevision revision, const CmImportResolver *imports,
    const CmHirModuleMap *modules, const CmUBodyDependency *dependencies,
    size_t dependency_count);

#endif
