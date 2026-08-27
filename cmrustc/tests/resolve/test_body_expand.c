#include "cm/driver/cfg.h"
#include "cm/resolve/body_expand.h"
#include "cm/resolve/imports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test-body-expand: %s\n", message);
        failures += 1;
    }
}

typedef struct Fixture {
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphResult result;
    CmImportResolver imports;
    CmSourceId root;
} Fixture;

static int fixture_init(Fixture *fixture, const char *path,
    const char *source)
{
    CmModuleGraphOptions options;
    cm_source_set_init(&fixture->sources);
    cm_module_graph_init(&fixture->graph);
    cm_cfg_set_init(&fixture->cfg);
    fixture->cfg.environment.target_os = "linux";
    fixture->cfg.environment.target_family = "unix";
    if (cm_source_add_memory(&fixture->sources, path,
            (const unsigned char *)source, strlen(source), &fixture->root)
            != CM_SOURCE_OK) {
        check(0, "could not add fixture source");
        return 0;
    }
    cm_module_graph_options_init(&options);
    options.cfg = &fixture->cfg;
    options.edition = CM_EDITION_2024;
    fixture->result = cm_module_graph_build(&fixture->graph,
        &fixture->sources, fixture->root, &options);
    if (fixture->result.error_count != 0u) {
        CmResolveError error;
        if (cm_module_graph_get_error(&fixture->graph, 0u, &error))
            fprintf(stderr, "test-body-expand: graph error: %s\n",
                cm_resolve_error_kind_name(error.kind));
        check(0, "fixture graph did not build");
        return 0;
    }
    cm_import_resolver_init(&fixture->imports);
    {
        CmImportResult imports = cm_import_resolve(&fixture->imports,
            &fixture->graph, fixture->result.revision);
        check(imports.error_count == 0u, "fixture imports did not resolve");
    }
    return 1;
}

static void fixture_destroy(Fixture *fixture)
{
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
}

static CmBodyExpandResult expand(Fixture *fixture)
{
    CmBodyExpandOptions options;
    cm_body_expand_options_init(&options);
    options.edition = CM_EDITION_2024;
    options.crate_identifier = "crate";
    options.cfg = &fixture->cfg;
    options.imports = &fixture->imports;
    return cm_body_expand_graph(&fixture->graph, fixture->result.revision,
        &options);
}

static int name_is(const CmAst *ast, CmInternId id, const char *expected)
{
    const CmInternedString *name = cm_ast_get_string(ast, id);
    size_t length = strlen(expected);
    return name != NULL && name->len == length
        && memcmp(name->bytes, expected, length) == 0;
}

static const CmAstItem *find_function(const CmAst *ast, const char *name)
{
    size_t index;
    for (index = 1u; index <= ast->items.len; ++index) {
        const CmAstItem *item = cm_ast_get_item(ast, (CmAstItemId)index);
        if (item != NULL && item->kind == CM_AST_ITEM_FUNCTION
            && name_is(ast, item->name, name)) return item;
    }
    return NULL;
}

static size_t count_kind(const CmAst *ast, CmAstExprId id,
    CmAstExprKind kind);

static size_t count_kind_stmt(const CmAst *ast, CmAstStmtId id,
    CmAstExprKind kind)
{
    const CmAstStmt *stmt = cm_ast_get_stmt(ast, id);
    if (stmt == NULL) return 0u;
    if (stmt->kind == CM_AST_STMT_LET)
        return count_kind(ast, stmt->data.let_stmt.initializer, kind)
            + count_kind(ast, stmt->data.let_stmt.else_block, kind);
    if (stmt->kind == CM_AST_STMT_EXPR)
        return count_kind(ast, stmt->data.expr_stmt.expression, kind);
    return 0u;
}

/* Counts expression nodes of `kind` reachable from `id` (common forms). */
static size_t count_kind(const CmAst *ast, CmAstExprId id,
    CmAstExprKind kind)
{
    const CmAstExpr *expr = cm_ast_get_expr(ast, id);
    size_t total = 0u;
    uint32_t index;
    if (expr == NULL) return 0u;
    if (expr->kind == kind) total += 1u;
    switch (expr->kind) {
    case CM_AST_EXPR_BLOCK:
    case CM_AST_EXPR_TRY_BLOCK:
        for (index = 0u; index < expr->data.block.statement_count; ++index)
            total += count_kind_stmt(ast, expr->data.block.statements[index],
                kind);
        return total + count_kind(ast, expr->data.block.tail, kind);
    case CM_AST_EXPR_CALL:
        total += count_kind(ast, expr->data.call.callee, kind);
        for (index = 0u; index < expr->data.call.argument_count; ++index)
            total += count_kind(ast, expr->data.call.arguments[index], kind);
        return total;
    case CM_AST_EXPR_METHOD_CALL:
        total += count_kind(ast, expr->data.method_call.receiver, kind);
        for (index = 0u; index < expr->data.method_call.argument_count;
                ++index)
            total += count_kind(ast,
                expr->data.method_call.arguments[index], kind);
        return total;
    case CM_AST_EXPR_UNARY:
        return total + count_kind(ast, expr->data.unary.operand, kind);
    case CM_AST_EXPR_BINARY:
    case CM_AST_EXPR_ASSIGN:
        return total + count_kind(ast, expr->data.binary.left, kind)
            + count_kind(ast, expr->data.binary.right, kind);
    case CM_AST_EXPR_IF:
        return total + count_kind(ast, expr->data.if_expr.condition, kind)
            + count_kind(ast, expr->data.if_expr.then_expr, kind)
            + count_kind(ast, expr->data.if_expr.else_expr, kind);
    case CM_AST_EXPR_MATCH:
        total += count_kind(ast, expr->data.match_expr.scrutinee, kind);
        for (index = 0u; index < expr->data.match_expr.arm_count; ++index)
            total += count_kind(ast, expr->data.match_expr.arms[index].body,
                kind);
        return total;
    case CM_AST_EXPR_TUPLE:
    case CM_AST_EXPR_ARRAY:
        for (index = 0u; index < expr->data.list.element_count; ++index)
            total += count_kind(ast, expr->data.list.elements[index], kind);
        return total;
    case CM_AST_EXPR_FIELD:
        return total + count_kind(ast, expr->data.field.base, kind);
    case CM_AST_EXPR_TUPLE_FIELD:
        return total + count_kind(ast, expr->data.tuple_field.base, kind);
    case CM_AST_EXPR_LOOP:
        return total + count_kind(ast, expr->data.loop_expr.body, kind);
    default:
        return total;
    }
}

static const char positive_source[] =
    "macro_rules! twice { ($e:expr) => { $e + $e }; }\n"
    "pub mod panic {\n"
    "    pub macro panic_2021 {\n"
    "        () => ({ $crate::panicking::panic_explicit() }),\n"
    "        ($($t:tt)+) => ({\n"
    "            $crate::panicking::panic_fmt("
                    "$crate::const_format_args!($($t)+))\n"
    "        }),\n"
    "    }\n"
    "}\n"
    "pub mod panicking {\n"
    "    pub fn panic_fmt(_a: u32) -> ! { loop {} }\n"
    "    pub fn panic(_s: &str) -> ! { loop {} }\n"
    "    pub fn panic_explicit() -> ! { loop {} }\n"
    "}\n"
    "pub fn user(x: u32) -> u32 {\n"
    "    macro_rules! local { ($a:expr) => { $a * 3 }; }\n"
    "    let y = twice!(x);\n"
    "    let z = local!(y);\n"
    "    assert!(z > 0, \"bad {}\", z);\n"
    "    assert!(y != 7);\n"
    "    if cfg!(target_os = \"linux\") {\n"
    "        panic!(\"value {} {name}\", z, name = y);\n"
    "    }\n"
    "    let _s: &str = stringify!(z + 1);\n"
    "    assert!(z < 9, concat!(\"limit \", stringify!(z), \" {}\"), z);\n"
    "    let _c: &str = concat!(\"a\", 1, stringify!(b + c), 'x');\n"
    "    z\n"
    "}\n"
    "impl Holder {\n"
    "    pub fn method(&self) -> u32 { twice!(self.0) }\n"
    "}\n"
    "pub struct Holder(pub u32);\n"
    "pub const LIMIT: u32 = twice!(4);\n"
    "#[macro_use]\n"
    "mod shared { macro_rules! shared { () => { 1u32 }; } }\n"
    "mod ub { macro_rules! pre { ($c:expr) => { if !$c { loop {} } }; }\n"
    "         pub(crate) use pre; }\n"
    "mod other {\n"
    "    use crate::ub::pre;\n"
    "    pub fn g(v: u32) -> u32 { pre!(v > 0); shared!() + twice!(v) }\n"
    "    pub mod deeper { pub fn h() -> u32 { shared!() } }\n"
    "}\n";

static void test_positive_chain(void)
{
    Fixture fixture;
    CmBodyExpandResult result;
    const CmAst *ast = NULL;
    const CmAstItem *user;
    const CmAstItem *method = NULL;
    CmModuleId root;
    size_t index;
    if (!fixture_init(&fixture, "body-expand/lib.rs", positive_source))
        return;
    result = expand(&fixture);
    check(result.failed == 0u, "positive fixture reported failures");
    if (result.failed != 0u)
        fprintf(stderr, "test-body-expand: first failure %s: %s\n",
            result.first_failure_macro, result.first_failure_reason);
    check(result.remaining_asm == 0u && result.remaining_builtin == 0u,
        "positive fixture left invocations behind");
    /* twice x4, local, panic_2021, pre, shared x2 */
    check(result.expanded_rules >= 9u, "macro_rules expansions missing");
    /* assert x3, cfg, panic, stringify, concat, const_format_args x3 */
    check(result.expanded_builtin >= 10u, "builtin expansions missing");
    check(cm_module_graph_get_root(&fixture.graph, &root)
        && cm_module_graph_borrow_ast(&fixture.graph, root, &ast)
        && ast != NULL, "root AST unavailable");
    if (ast != NULL) {
        user = find_function(ast, "user");
        for (index = 1u; index <= ast->items.len && method == NULL; ++index) {
            const CmAstItem *item = cm_ast_get_item(ast, (CmAstItemId)index);
            if (item != NULL && item->kind == CM_AST_ITEM_FUNCTION
                && name_is(ast, item->name, "method")) method = item;
        }
        check(user != NULL && method != NULL, "fixture functions missing");
        if (user != NULL) {
            CmAstExprId body = user->data.function_item.body;
            check(count_kind(ast, body, CM_AST_EXPR_MACRO) == 0u,
                "user body still contains macro invocations");
            /* twice/local became binary arithmetic; panic! became a call
             * through the generated match/new_v1 chain. */
            check(count_kind(ast, body, CM_AST_EXPR_BINARY) >= 4u,
                "expanded arithmetic missing");
            check(count_kind(ast, body, CM_AST_EXPR_MATCH) >= 1u,
                "format_args match missing");
            check(count_kind(ast, body, CM_AST_EXPR_IF) >= 3u,
                "assert conditionals missing");
        }
        if (method != NULL)
            check(count_kind(ast, method->data.function_item.body,
                    CM_AST_EXPR_MACRO) == 0u,
                "impl method body still contains macro invocations");
    }
    fixture_destroy(&fixture);
}

static const char negative_source[] =
    "pub fn user() -> u32 {\n"
    "    let a = nope!(1);\n"
    "    unsafe { asm!(\"nop\") };\n"
    "    a\n"
    "}\n";

static void test_negative_counts(void)
{
    Fixture fixture;
    CmBodyExpandResult result;
    if (!fixture_init(&fixture, "body-expand-negative/lib.rs",
            negative_source)) return;
    result = expand(&fixture);
    check(result.failed == 1u, "unknown macro was not counted as failed");
    check(strcmp(result.first_failure_macro, "nope") == 0,
        "first failure did not name the unknown macro");
    check(strcmp(result.first_failure_reason, "macro is not in scope") == 0,
        "first failure reason unexpected");
    check(result.remaining_asm == 1u, "asm! was not retained");
    check(result.expanded_rules == 0u && result.expanded_builtin == 0u,
        "negative fixture expanded something");
    fixture_destroy(&fixture);
}

int main(void)
{
    test_positive_chain();
    test_negative_counts();
    if (failures != 0) {
        fprintf(stderr, "body expansion tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("body expansion tests: ok\n");
    return 0;
}
