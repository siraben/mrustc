#include "cm/driver/cfg.h"
#include "cm/hir/lower.h"
#include "cm/hir/ubody.h"
#include "cm/resolve/body_expand.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test-ubody: %s\n", message);
        failures += 1;
    }
}

typedef struct Fixture {
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerResult lower_result;
    CmUBodySet bodies;
    CmUBodyLowerResult ubody_result;
    CmSourceId root;
} Fixture;

static int fixture_init(Fixture *fixture, const char *source)
{
    CmModuleGraphOptions graph_options;
    CmHirLowerOptions lower_options;
    CmBodyExpandOptions expand_options;
    CmImportResult import_result;

    memset(fixture, 0, sizeof(*fixture));
    cm_source_set_init(&fixture->sources);
    cm_module_graph_init(&fixture->graph);
    cm_cfg_set_init(&fixture->cfg);
    fixture->cfg.environment.target_os = "linux";
    fixture->cfg.environment.target_family = "unix";
    if (cm_source_add_memory(&fixture->sources, "ubody/lib.rs",
            (const unsigned char *)source, strlen(source), &fixture->root)
            != CM_SOURCE_OK) {
        check(0, "could not add fixture source");
        return 0;
    }
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &fixture->cfg;
    graph_options.edition = CM_EDITION_2024;
    fixture->graph_result = cm_module_graph_build(&fixture->graph,
        &fixture->sources, fixture->root, &graph_options);
    if (fixture->graph_result.error_count != 0u) {
        CmResolveError error;
        if (cm_module_graph_get_error(&fixture->graph, 0u, &error)) {
            char detail[128];
            if (!cm_module_graph_copy_string(&fixture->graph, error.detail_a,
                    detail, sizeof(detail))) detail[0] = '\0';
            fprintf(stderr, "test-ubody: graph error: %s at %lu:%lu: %s\n",
                cm_resolve_error_kind_name(error.kind),
                (unsigned long)error.line, (unsigned long)error.column,
                detail);
        }
        check(0, "fixture graph did not build");
        return 0;
    }
    cm_import_resolver_init(&fixture->imports);
    import_result = cm_import_resolve(&fixture->imports, &fixture->graph,
        fixture->graph_result.revision);
    check(import_result.error_count == 0u, "fixture imports did not resolve");
    cm_body_expand_options_init(&expand_options);
    expand_options.cfg = &fixture->cfg;
    expand_options.imports = &fixture->imports;
    (void)cm_body_expand_graph(&fixture->graph,
        fixture->graph_result.revision, &expand_options);
    cm_hir_context_init(&fixture->hir);
    cm_hir_module_map_init(&fixture->map);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "ubody";
    lower_options.source = fixture->root;
    lower_options.edition = CM_HIR_EDITION_2024;
    lower_options.pointer_bits = 64u;
    fixture->lower_result = cm_hir_lower_module_graph(&fixture->hir,
        &fixture->graph, fixture->graph_result.revision, &fixture->imports,
        &fixture->map, &lower_options);
    if (fixture->lower_result.error_count != 0u) {
        fprintf(stderr, "test-ubody: HIR: %s: %s\n",
            cm_hir_lower_error_kind_name(
                fixture->lower_result.first_error.kind),
            fixture->lower_result.first_error.message);
        check(0, "fixture HIR did not lower");
        return 0;
    }
    cm_ubody_set_init(&fixture->bodies);
    fixture->ubody_result = cm_ubody_lower_all(&fixture->bodies,
        &fixture->hir, &fixture->graph, fixture->graph_result.revision,
        &fixture->imports, &fixture->map, NULL, 0u);
    return 1;
}

static void fixture_destroy(Fixture *fixture)
{
    cm_ubody_set_destroy(&fixture->bodies);
    cm_hir_module_map_destroy(&fixture->map);
    cm_hir_context_destroy(&fixture->hir);
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
}

static const CmUBody *body_of(const Fixture *fixture, const char *name)
{
    size_t index;
    for (index = 0u; index < fixture->hir.items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &fixture->hir.items, index);
        const CmInternedString *item_name;
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION) continue;
        item_name = cm_interner_get(&fixture->hir.strings, item->name);
        if (item_name == NULL || item_name->len != strlen(name)
            || memcmp(item_name->bytes, name, item_name->len) != 0) continue;
        return cm_ubody_get(&fixture->bodies,
            item->data.function_item.body);
    }
    return NULL;
}

static size_t count_expr_kind(const CmUBody *body, CmUExprKind kind)
{
    size_t index;
    size_t total = 0u;
    for (index = 0u; index < body->expressions.len; ++index) {
        const CmUExpr *expr = (const CmUExpr *)cm_vec_at_const(
            &body->expressions, index);
        if (expr->kind == kind) total += 1u;
    }
    return total;
}

static size_t count_pat_kind(const CmUBody *body, CmUPatKind kind)
{
    size_t index;
    size_t total = 0u;
    for (index = 0u; index < body->patterns.len; ++index) {
        const CmUPat *pat = (const CmUPat *)cm_vec_at_const(&body->patterns,
            index);
        if (pat->kind == kind) total += 1u;
    }
    return total;
}

static size_t count_resolution(const CmUBody *body, CmUResolutionKind kind)
{
    size_t index;
    size_t total = 0u;
    for (index = 0u; index < body->expressions.len; ++index) {
        const CmUExpr *expr = (const CmUExpr *)cm_vec_at_const(
            &body->expressions, index);
        if (expr->kind == CM_U_EXPR_PATH
            && expr->data.path.resolution.kind == kind) total += 1u;
        if (expr->kind == CM_U_EXPR_STRUCT
            && expr->data.struct_expr.resolution.kind == kind) total += 1u;
    }
    for (index = 0u; index < body->patterns.len; ++index) {
        const CmUPat *pat = (const CmUPat *)cm_vec_at_const(&body->patterns,
            index);
        if ((pat->kind == CM_U_PAT_PATH
                && pat->data.path.resolution.kind == kind)
            || ((pat->kind == CM_U_PAT_TUPLE_STRUCT
                    || pat->kind == CM_U_PAT_STRUCT)
                && pat->data.struct_pat.resolution.kind == kind))
            total += 1u;
    }
    return total;
}

static const char fixture_source[] =
    "pub enum Choice { None, Some(u32), Named { value: u32 } }\n"
    "use crate::Choice::None;\n"
    "pub struct Point { pub x: u32, pub y: u32 }\n"
    "pub const LIMIT: u32 = 7;\n"
    "pub trait Make { fn make() -> Self; }\n"
    "pub fn helper((a, b): (u32, u32)) -> u32 { a + b }\n"
    "impl Point {\n"
    "    pub fn new(x: u32, y: u32) -> Self { Self { x, y } }\n"
    "    /// Documented, attributed method.\n"
    "    #[inline]\n"
    "    pub fn sum(&self, other: u32) -> u32 {\n"
    "        let Point { x, y: yy } = *self;\n"
    "        x + yy + other\n"
    "    }\n"
    "}\n"
    "pub fn generic<T: Make + Default>(count: usize) -> u32 {\n"
    "    let _t = T::make();\n"
    "    let _d: T = T::default();\n"
    "    let mut total = u32::MAX - LIMIT;\n"
    "    let mut i = 0usize;\n"
    "    'outer: loop {\n"
    "        i += 1;\n"
    "        if i == 3 { continue 'outer; }\n"
    "        if i > count { break 'outer; }\n"
    "        total -= 1;\n"
    "    }\n"
    "    while i < count { i += 1; }\n"
    "    for k in 0..count { total = total.wrapping_add(k as u32); }\n"
    "    let c = Choice::Some(total);\n"
    "    let n = match c {\n"
    "        Choice::Some(v) if v > 1 => v,\n"
    "        Choice::Named { value, .. } => value,\n"
    "        None => 0,\n"
    "        _ => 1,\n"
    "    };\n"
    "    let closure = move |q: u32, r| q + r + n;\n"
    "    let arr = [0u32; 4];\n"
    "    let tup = (1u8, 2i64, 'c', \"s\", b'x', 1.5f32, true);\n"
    "    let rf = &&tup;\n"
    "    let raw = &raw const arr;\n"
    "    let _ = (rf, raw, closure(1, 2), arr[0], tup.1);\n"
    "    if let Choice::Some(w) = c { total += w; }\n"
    "    let val = loop { break 5u32; };\n"
    "    fn nested(a: u32) -> u32 { a * 2 }\n"
    "    let p = Point::new(1, 2);\n"
    "    let q = Point { x: 3, ..p };\n"
    "    total + val + helper((nested(q.x), 1)) + p.sum(2) - 0x1F_u32\n"
    "        + <Point as Make>::make().x\n"
    "}\n"
    "impl Make for Point { fn make() -> Self { Point::new(0, 0) } }\n"
    "impl Default for Point { fn default() -> Self { Point::new(0, 0) } }\n"
    "pub trait Default { fn default() -> Self; }\n"
    "pub fn with_asm() { unsafe { asm!(\"nop\") } }\n";

static void test_lowering(void)
{
    Fixture fixture;
    const CmUBody *generic;
    const CmUBody *sum;
    const CmUBody *asm_body;
    if (!fixture_init(&fixture, fixture_source)) return;
    check(fixture.ubody_result.failed == 0u, "some body failed to lower");
    if (fixture.ubody_result.failed != 0u)
        fprintf(stderr, "test-ubody: first failure body %lu: %s\n",
            (unsigned long)fixture.ubody_result.first_failure_body,
            fixture.ubody_result.first_failure);
    check(fixture.ubody_result.lowered >= 8u, "too few bodies lowered");
    check(fixture.ubody_result.nested_items == 1u, "nested item not counted");
    check(fixture.ubody_result.retained_macros == 1u, "asm! not retained");
    generic = body_of(&fixture, "generic");
    sum = body_of(&fixture, "sum");
    asm_body = body_of(&fixture, "with_asm");
    check(generic != NULL && sum != NULL && asm_body != NULL,
        "fixture bodies missing");
    if (generic != NULL) {
        check(generic->status == CM_U_BODY_LOWERED, "generic not lowered");
        check(generic->parameter_count == 1u, "generic parameter count");
        {
            size_t index;
            for (index = 0u; index < generic->expressions.len; ++index) {
                const CmUExpr *expr = (const CmUExpr *)cm_vec_at_const(
                    &generic->expressions, index);
                const CmInternId *segments = NULL;
                uint32_t count = 0u;
                uint32_t segment;
                if (expr->kind == CM_U_EXPR_PATH && expr->data.path
                        .resolution.kind == CM_U_RESOLVED_UNRESOLVED) {
                    segments = expr->data.path.segments;
                    count = expr->data.path.segment_count;
                } else if (expr->kind == CM_U_EXPR_STRUCT
                    && expr->data.struct_expr.resolution.kind
                        == CM_U_RESOLVED_UNRESOLVED) {
                    fprintf(stderr, "test-ubody: unresolved struct path\n");
                }
                if (segments == NULL) continue;
                fprintf(stderr, "test-ubody: unresolved path:");
                for (segment = 0u; segment < count; ++segment) {
                    const CmInternedString *name = cm_interner_get(
                        &fixture.bodies.strings, segments[segment]);
                    fprintf(stderr, " %.*s", name == NULL ? 0
                        : (int)name->len, name == NULL ? ""
                        : (const char *)name->bytes);
                }
                fprintf(stderr, "\n");
            }
        }
        {
            size_t index;
            for (index = 0u; index < generic->patterns.len; ++index) {
                const CmUPat *pat = (const CmUPat *)cm_vec_at_const(
                    &generic->patterns, index);
                if ((pat->kind == CM_U_PAT_PATH && pat->data.path.resolution
                        .kind == CM_U_RESOLVED_UNRESOLVED)
                    || ((pat->kind == CM_U_PAT_TUPLE_STRUCT
                            || pat->kind == CM_U_PAT_STRUCT)
                        && pat->data.struct_pat.resolution.kind
                            == CM_U_RESOLVED_UNRESOLVED))
                    fprintf(stderr, "test-ubody: unresolved pattern kind=%d "
                        "at %lu..%lu\n", (int)pat->kind,
                        (unsigned long)pat->span.start,
                        (unsigned long)pat->span.end);
            }
        }
        check(count_resolution(generic, CM_U_RESOLVED_UNRESOLVED) == 0u,
            "generic body left paths unresolved");
        check(count_resolution(generic, CM_U_RESOLVED_GENERIC_PARAM) == 2u,
            "T::make / T::default not resolved as generic prefixes");
        check(count_resolution(generic, CM_U_RESOLVED_PRIMITIVE) == 1u,
            "u32::MAX not resolved as a primitive prefix");
        check(count_resolution(generic, CM_U_RESOLVED_VARIANT) >= 4u,
            "Choice variants (incl. bare `None` pattern) not resolved");
        check(count_resolution(generic, CM_U_RESOLVED_TYPE_ASSOC) >= 1u,
            "Point::new not resolved as type + associated tail");
        check(count_resolution(generic, CM_U_RESOLVED_NESTED_ITEM) == 1u,
            "nested fn not resolved as a body-local item");
        check(count_expr_kind(generic, CM_U_EXPR_WHILE) == 1u
            && count_expr_kind(generic, CM_U_EXPR_FOR) == 1u
            && count_expr_kind(generic, CM_U_EXPR_LOOP) == 2u
            && count_expr_kind(generic, CM_U_EXPR_CLOSURE) == 1u
            && count_expr_kind(generic, CM_U_EXPR_MATCH) == 1u
            && count_expr_kind(generic, CM_U_EXPR_QUALIFIED_PATH) == 1u
            && count_expr_kind(generic, CM_U_EXPR_ARRAY_REPEAT) == 1u
            && count_expr_kind(generic, CM_U_EXPR_ASSIGN_OP) >= 3u
            && count_expr_kind(generic, CM_U_EXPR_CAST) == 1u
            && count_expr_kind(generic, CM_U_EXPR_RANGE) == 1u
            && count_expr_kind(generic, CM_U_EXPR_CONTINUE) == 1u
            && count_expr_kind(generic, CM_U_EXPR_BREAK) == 2u,
            "generic body expression kinds");
        /* `&&tup` becomes two REF nodes; `&raw const` one raw REF. */
        check(count_expr_kind(generic, CM_U_EXPR_REF) == 3u,
            "reference forms");
        check(count_pat_kind(generic, CM_U_PAT_TUPLE_STRUCT) == 2u
            && count_pat_kind(generic, CM_U_PAT_STRUCT) == 1u
            && count_pat_kind(generic, CM_U_PAT_WILD) == 2u
            && count_pat_kind(generic, CM_U_PAT_PATH) == 1u,
            "pattern kinds");
        check(count_expr_kind(generic, CM_U_EXPR_UNSUPPORTED) == 0u,
            "unsupported expression in generic body");
    }
    if (sum != NULL) {
        check(sum->parameter_count == 2u, "sum parameter count");
        check(sum->locals.len == 4u, "sum locals: self, other, x, yy");
        check(count_resolution(sum, CM_U_RESOLVED_LOCAL) == 4u,
            "sum local references");
    }
    if (asm_body != NULL)
        check(count_expr_kind(asm_body, CM_U_EXPR_ASM) == 1u,
            "asm node missing");
    fixture_destroy(&fixture);
}

int main(void)
{
    test_lowering();
    if (failures != 0) {
        fprintf(stderr, "ubody tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("ubody tests: ok\n");
    return 0;
}
