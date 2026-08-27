#include "cm/driver/cfg.h"
#include "cm/hir/lower.h"
#include "cm/hir/tyck.h"
#include "cm/hir/ubody.h"
#include "cm/resolve/body_expand.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "test-tyck: %s\n", message);
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
    CmUBodySet bodies;
    CmTyckSet tyck;
    CmTyckResult result;
    CmSourceId root;
} Fixture;

static int fixture_init(Fixture *fixture, const char *source)
{
    CmModuleGraphOptions graph_options;
    CmHirLowerOptions lower_options;
    CmBodyExpandOptions expand_options;
    CmHirLowerResult lower_result;

    memset(fixture, 0, sizeof(*fixture));
    cm_source_set_init(&fixture->sources);
    cm_module_graph_init(&fixture->graph);
    cm_cfg_set_init(&fixture->cfg);
    fixture->cfg.environment.target_os = "linux";
    fixture->cfg.environment.target_family = "unix";
    if (cm_source_add_memory(&fixture->sources, "tyck/lib.rs",
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
        if (cm_module_graph_get_error(&fixture->graph, 0u, &error))
            fprintf(stderr, "test-tyck: graph error: %s at %lu:%lu\n",
                cm_resolve_error_kind_name(error.kind),
                (unsigned long)error.line, (unsigned long)error.column);
        check(0, "fixture graph did not build");
        return 0;
    }
    cm_import_resolver_init(&fixture->imports);
    (void)cm_import_resolve(&fixture->imports, &fixture->graph,
        fixture->graph_result.revision);
    cm_body_expand_options_init(&expand_options);
    expand_options.cfg = &fixture->cfg;
    expand_options.imports = &fixture->imports;
    (void)cm_body_expand_graph(&fixture->graph,
        fixture->graph_result.revision, &expand_options);
    cm_hir_context_init(&fixture->hir);
    cm_hir_module_map_init(&fixture->map);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "tyck";
    lower_options.source = fixture->root;
    lower_options.edition = CM_HIR_EDITION_2024;
    lower_options.pointer_bits = 64u;
    lower_result = cm_hir_lower_module_graph(&fixture->hir, &fixture->graph,
        fixture->graph_result.revision, &fixture->imports, &fixture->map,
        &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "test-tyck: HIR: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
        check(0, "fixture HIR did not lower");
        return 0;
    }
    cm_ubody_set_init(&fixture->bodies);
    (void)cm_ubody_lower_all(&fixture->bodies, &fixture->hir, &fixture->graph,
        fixture->graph_result.revision, &fixture->imports, &fixture->map);
    cm_tyck_set_init(&fixture->tyck);
    fixture->result = cm_tyck_all(&fixture->tyck, &fixture->hir,
        &fixture->bodies, &fixture->graph, fixture->graph_result.revision,
        &fixture->imports, &fixture->map);
    return 1;
}

static void fixture_destroy(Fixture *fixture)
{
    cm_tyck_set_destroy(&fixture->tyck);
    cm_ubody_set_destroy(&fixture->bodies);
    cm_hir_module_map_destroy(&fixture->map);
    cm_hir_context_destroy(&fixture->hir);
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
}

static const CmHirItem *function_named(const Fixture *fixture,
    const char *name)
{
    size_t index;
    for (index = 0u; index < fixture->hir.items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &fixture->hir.items, index);
        const CmInternedString *item_name;
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION) continue;
        /* Skip bodyless trait declarations that share the name. */
        if (item->data.function_item.body == CM_HIR_BODY_NONE) continue;
        item_name = cm_interner_get(&fixture->hir.strings, item->name);
        if (item_name != NULL && item_name->len == strlen(name)
            && memcmp(item_name->bytes, name, item_name->len) == 0)
            return item;
    }
    return NULL;
}

/* Render the type of the body's root expression. */
static void root_type(Fixture *fixture, const char *name, CmStrBuf *out)
{
    const CmHirItem *item = function_named(fixture, name);
    const CmUBody *ub;
    const CmTyckBody *tb;
    cm_str_buf_clear(out);
    if (item == NULL) return;
    ub = cm_ubody_get(&fixture->bodies, item->data.function_item.body);
    tb = cm_tyck_get(&fixture->tyck, item->data.function_item.body);
    if (ub == NULL || tb == NULL || tb->expr_types == NULL) return;
    cm_ty_print(&fixture->tyck.arena, &fixture->hir,
        tb->expr_types[ub->root], out);
}

static int body_typed(Fixture *fixture, const char *name)
{
    const CmHirItem *item = function_named(fixture, name);
    const CmTyckBody *tb;
    if (item == NULL) return 0;
    tb = cm_tyck_get(&fixture->tyck, item->data.function_item.body);
    if (tb != NULL && tb->status != CM_TYCK_BODY_TYPED)
        fprintf(stderr, "test-tyck: %s: %lu unresolved, %lu errors (%s)\n",
            name, (unsigned long)tb->unresolved_nodes,
            (unsigned long)tb->error_nodes,
            tb->first_error == NULL ? "-" : tb->first_error);
    return tb != NULL && tb->status == CM_TYCK_BODY_TYPED;
}

static const char fixture_source[] =
    "pub struct Point { pub x: u32, pub y: u32 }\n"
    "pub struct Pair<T>(pub T, pub T);\n"
    "pub enum Opt<T> { None, Some(T) }\n"
    "use crate::Opt::{None, Some};\n"
    "pub trait Shape { fn area(&self) -> u32; fn double(&self) -> u32 {\n"
    "    self.area() * 2 } }\n"
    "impl Point {\n"
    "    pub fn new(x: u32, y: u32) -> Point { Point { x, y } }\n"
    "    pub fn sum(&self) -> u32 { self.x + self.y }\n"
    "    pub fn scaled(self, k: u32) -> Self { Self { x: self.x * k, y: self.y * k } }\n"
    "}\n"
    "impl Shape for Point { fn area(&self) -> u32 { self.x * self.y } }\n"
    "pub trait Dup {}\n"
    "impl Dup for u8 {}\n"
    "impl<T: Dup> Pair<T> { pub fn first(&self) -> T { self.0 } }\n"
    "pub fn helper(v: u32) -> u32 { v + 1 }\n"
    "pub fn basic() -> u32 {\n"
    "    let p = Point::new(1, 2);\n"
    "    let q = p.scaled(3);\n"
    "    let mut total = q.sum() + helper(4) + q.double();\n"
    "    let pair = Pair(5u8, 6);\n"
    "    let f: u8 = pair.first();\n"
    "    total += f as u32;\n"
    "    let arr = [1u32, 2, 3];\n"
    "    for v in &arr { total += *v; }\n"
    "    let o = Some(total);\n"
    "    match o { Some(v) if v > 0 => v, Some(_) => 0, None => 1 }\n"
    "}\n"
    "pub fn generic<T: Shape>(s: &T, flag: bool) -> u32 {\n"
    "    if flag { s.area() } else { s.double() }\n"
    "}\n"
    "pub fn literals() -> (i64, f32, bool, char, &'static str) {\n"
    "    let x = 7;\n"
    "    let y = 2.5;\n"
    "    (x, y, x > 3, 'c', \"s\")\n"
    "}\n";

static void test_inference(void)
{
    Fixture fixture;
    CmStrBuf text;
    if (!fixture_init(&fixture, fixture_source)) return;
    cm_str_buf_init(&text);
    check(fixture.result.skipped == 0u, "bodies skipped");
    check(body_typed(&fixture, "basic"), "basic body not fully typed");
    check(body_typed(&fixture, "generic"), "generic body not fully typed");
    check(body_typed(&fixture, "literals"), "literals body not fully typed");
    check(body_typed(&fixture, "sum") && body_typed(&fixture, "scaled")
        && body_typed(&fixture, "first") && body_typed(&fixture, "double"),
        "method bodies not fully typed");
    root_type(&fixture, "basic", &text);
    check(strcmp(cm_str_buf_c_str(&text), "u32") == 0, "basic root type");
    root_type(&fixture, "literals", &text);
    check(strcmp(cm_str_buf_c_str(&text), "(i64, f32, bool, char, &str)")
        == 0, "literals root type");
    if (failures != 0)
        fprintf(stderr, "test-tyck: literals root = %s\n",
            cm_str_buf_c_str(&text));
    cm_str_buf_destroy(&text);
    fixture_destroy(&fixture);
}

/*
 * Items generated by item-position macro_rules resolve through bindings
 * (glob and direct paths), and body-local `use` declarations bind names,
 * aliases, and enum-variant globs inside one body.
 */
static const char generated_source[] =
    "pub mod simd {\n"
    "    macro_rules! simd_ty {\n"
    "        ($id:ident) => { pub struct $id(pub u32);\n"
    "            impl $id { pub fn splat(v: u32) -> Self { $id(v) } } };\n"
    "    }\n"
    "    simd_ty!(i32x4);\n"
    "}\n"
    "pub mod user {\n"
    "    use crate::simd::*;\n"
    "    pub fn make() -> i32x4 { i32x4::splat(7) }\n"
    "}\n"
    "pub fn direct() -> u32 { crate::simd::i32x4::splat(9).0 }\n"
    "pub enum Kind { Alpha, Beta }\n"
    "pub struct Wide(pub u64);\n"
    "pub fn pick(flag: bool) -> Kind {\n"
    "    use self::Kind::*;\n"
    "    use crate::Wide as W;\n"
    "    let w = W(3);\n"
    "    if flag && w.0 > 2 { Alpha } else { Beta }\n"
    "}\n"
    "pub fn cfgd() -> Wide {\n"
    "    #[cfg(not(feature = \"optimize_for_size\"))]\n"
    "    {\n"
    "        Wide(1)\n"
    "    }\n"
    "    #[cfg(feature = \"optimize_for_size\")]\n"
    "    {\n"
    "        missing_helper(2)\n"
    "    }\n"
    "}\n";

static void test_generated_and_body_use(void)
{
    Fixture fixture;
    if (!fixture_init(&fixture, generated_source)) return;
    check(fixture.result.unresolved_nodes == 0u,
        "generated/body-use fixture left unresolved nodes");
    check(body_typed(&fixture, "make"),
        "glob of macro-generated item not typed");
    check(body_typed(&fixture, "direct"),
        "direct path to macro-generated item not typed");
    check(body_typed(&fixture, "pick"),
        "body-local use not typed");
    check(body_typed(&fixture, "cfgd"),
        "cfg-inactive body statements not stripped");
    fixture_destroy(&fixture);
}

/*
 * Selection and lowering regressions: slice `rest @ ..` binds the
 * subslice; integer literals take method calls (`999.eq2(&3)` is not a
 * float); qualified associated calls and binary operators pick impls by
 * argument types; field access derefs through user `Deref`.
 */
static const char selection_source[] =
    "pub trait Eq2 { fn eq2(&self, o: &Self) -> bool; }\n"
    "impl Eq2 for i32 { fn eq2(&self, _o: &i32) -> bool { true } }\n"
    "pub fn lit_method() -> bool { 999.eq2(&3) }\n"
    "pub fn takes(d: &[u8]) -> u8 { takes(d) }\n"
    "pub fn walk(src: &[u8]) -> u8 {\n"
    "    let mut digits = src;\n"
    "    while let [c, rest @ ..] = digits {\n"
    "        let _x = *c;\n"
    "        digits = rest;\n"
    "        return takes(rest);\n"
    "    }\n"
    "    0\n"
    "}\n"
    "pub struct NZ(pub u32);\n"
    "pub trait From2<T> { fn from2(v: T) -> Self; }\n"
    "impl From2<NZ> for u64 { fn from2(_v: NZ) -> u64 { 1 } }\n"
    "impl From2<u32> for u64 { fn from2(_v: u32) -> u64 { 2 } }\n"
    "pub fn qcall(mant: u32) -> u64 { <u64>::from2(mant) }\n"
    "pub struct W2<T>(pub T);\n"
    "pub trait BOr<R = Self> { type Output; fn bitor(self, r: R) -> Self::Output; }\n"
    "pub trait BOrA<R = Self> { fn bora(&mut self, r: R); }\n"
    "impl<T> BOr<NZ> for W2<T> { type Output = NZ; fn bitor(self, r: NZ) -> NZ { r } }\n"
    "impl BOr for W2<u8> {\n"
    "    type Output = W2<u8>;\n"
    "    fn bitor(self, o: W2<u8>) -> W2<u8> { o }\n"
    "}\n"
    "impl BOrA for W2<u8> {\n"
    "    fn bora(&mut self, o: W2<u8>) { *self = *self | o; }\n"
    "}\n"
    "pub struct Waker2 { pub data: u32 }\n"
    "pub struct MD<T> { value: T }\n"
    "pub trait Deref { type Target; fn deref(&self) -> &Self::Target; }\n"
    "impl<T> Deref for MD<T> {\n"
    "    type Target = T;\n"
    "    fn deref(&self) -> &T { &self.value }\n"
    "}\n"
    "pub fn wake2(w: &MD<Waker2>) -> u32 { w.data }\n";

static void test_selection(void)
{
    Fixture fixture;
    if (!fixture_init(&fixture, selection_source)) return;
    check(body_typed(&fixture, "lit_method"),
        "integer literal method call not typed");
    check(body_typed(&fixture, "walk"),
        "slice rest binding not typed as subslice");
    check(body_typed(&fixture, "qcall"),
        "qualified call did not pick the matching impl");
    check(body_typed(&fixture, "bora"),
        "operator did not pick the matching impl");
    check(body_typed(&fixture, "wake2"),
        "field access through user Deref not typed");
    fixture_destroy(&fixture);
}

int main(void)
{
    test_inference();
    test_generated_and_body_use();
    test_selection();
    if (failures != 0) {
        fprintf(stderr, "tyck tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("tyck tests: ok\n");
    return 0;
}
