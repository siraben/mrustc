#include "cm/hir/body.h"
#include "cm/alloc.h"
#include "cm/hir/lower.h"
#include "cm/hir/module_map.h"
#include "cm/resolve/imports.h"
#include "cm/resolve/module_graph.h"
#include "cm/source.h"

#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TestFixture {
    CmSourceSet sources;
    CmSourceId source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirBodyId body;
} TestFixture;

static jmp_buf oom_jump;

static char *read_dump(FILE *stream);

static void jump_on_oom(size_t requested_size, void *context)
{
    (void)requested_size;
    (void)context;
    longjmp(oom_jump, 1);
}

static const CmHirItem *find_function(const CmHirContext *hir,
    const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmInternedString *item_name;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION) continue;
        item_name = cm_interner_get(&hir->strings, item->name);
        if (item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static const CmHirItem *find_value(const CmHirContext *hir,
    CmHirItemKind kind, const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmInternedString *item_name;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL || item->kind != kind) continue;
        item_name = cm_interner_get(&hir->strings, item->name);
        if (item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static const CmHirItem *find_impl_function(const CmHirContext *hir,
    const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmHirItem *parent;
        const CmHirDefinition *parent_definition;
        const CmInternedString *item_name;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        parent_definition = item == NULL
            || item->kind != CM_HIR_ITEM_FUNCTION
            || cm_hir_def_id_is_none(item->parent_definition)
            ? NULL : cm_hir_lookup_definition(hir,
                item->parent_definition);
        parent = parent_definition == NULL
                || parent_definition->kind != CM_HIR_DEFINITION_ITEM
                || parent_definition->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir,
                parent_definition->entity.item_id);
        if (parent == NULL || parent->kind != CM_HIR_ITEM_IMPL) continue;
        item_name = cm_interner_get(&hir->strings, item->name);
        if (item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static const CmHirItem *find_trait_function(const CmHirContext *hir,
    const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmHirItem *parent;
        const CmHirDefinition *parent_definition;
        const CmInternedString *item_name;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        parent_definition = item == NULL
                || item->kind != CM_HIR_ITEM_FUNCTION
                || cm_hir_def_id_is_none(item->parent_definition)
            ? NULL : cm_hir_lookup_definition(hir,
                item->parent_definition);
        parent = parent_definition == NULL
                || parent_definition->kind != CM_HIR_DEFINITION_ITEM
                || parent_definition->state != CM_HIR_DEFINITION_BOUND
            ? NULL : cm_hir_get_item(hir,
                parent_definition->entity.item_id);
        if (parent == NULL || parent->kind != CM_HIR_ITEM_TRAIT) continue;
        item_name = cm_interner_get(&hir->strings, item->name);
        if (item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static const CmHirItem *find_struct(const CmHirContext *hir,
    const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmInternedString *item_name;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_STRUCT) continue;
        item_name = cm_interner_get(&hir->strings, item->name);
        if (item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static const CmHirItem *find_trait(const CmHirContext *hir,
    const char *name)
{
    size_t index;
    size_t length;

    length = strlen(name);
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        const CmInternedString *item_name;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_TRAIT) continue;
        item_name = cm_interner_get(&hir->strings, item->name);
        if (item_name != NULL && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) {
            return item;
        }
    }
    return NULL;
}

static int source_span_is(const char *source, CmSpan span,
    const char *expected)
{
    size_t length;

    length = strlen(expected);
    return span.end >= span.start
        && (size_t)(span.end - span.start) == length
        && memcmp(source + span.start, expected, length) == 0;
}

static int hir_name_is(const CmHirContext *hir, CmInternId name,
    const char *expected)
{
    const CmInternedString *text;
    size_t length;

    text = cm_interner_get(&hir->strings, name);
    length = strlen(expected);
    return text != NULL && text->len == length
        && memcmp(text->bytes, expected, length) == 0;
}

static size_t bool_type_count(const CmHirContext *hir,
    CmHirTypeId *out_first)
{
    size_t count;
    size_t index;

    count = 0u;
    if (out_first != NULL) *out_first = CM_HIR_TYPE_NONE;
    for (index = 0u; index < hir->types.len; ++index) {
        const CmHirType *type;

        type = (const CmHirType *)cm_vec_at_const(&hir->types, index);
        if (type == NULL || type->kind != CM_HIR_TYPE_BOOL_KIND) continue;
        if (count == 0u && out_first != NULL) {
            *out_first = (CmHirTypeId)(index + 1u);
        }
        ++count;
    }
    return count;
}

static int hir_type_is_usize(const CmHirContext *hir, CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_USIZE;
}

static void fixture_init_named(TestFixture *fixture, const char *source,
    const char *function_name)
{
    CmModuleGraphOptions graph_options;
    CmImportResult import_result;
    CmHirLowerOptions options;
    CmHirLowerResult lower_result;
    const CmHirItem *function;

    memset(fixture, 0, sizeof(*fixture));
    cm_source_set_init(&fixture->sources);
    assert(cm_source_add_memory(&fixture->sources, "body/lib.rs",
        (const unsigned char *)source, strlen(source), &fixture->source)
        == CM_SOURCE_OK);
    cm_cfg_set_init(&fixture->cfg);
    cm_module_graph_init(&fixture->graph);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2021;
    graph_options.cfg = &fixture->cfg;
    fixture->graph_result = cm_module_graph_build(&fixture->graph,
        &fixture->sources, fixture->source, &graph_options);
    assert(fixture->graph_result.error_count == 0u
        && fixture->graph_result.revision != CM_MODULE_GRAPH_REVISION_NONE);
    cm_import_resolver_init(&fixture->imports);
    import_result = cm_import_resolve(&fixture->imports, &fixture->graph,
        fixture->graph_result.revision);
    assert(import_result.error_count == 0u);
    cm_hir_context_init(&fixture->hir);
    cm_hir_module_map_init(&fixture->map);
    cm_hir_lower_options_init(&options);
    options.crate_name = "body_test";
    options.edition = CM_HIR_EDITION_2021;
    lower_result = cm_hir_lower_module_graph(&fixture->hir,
        &fixture->graph, fixture->graph_result.revision, &fixture->imports,
        &fixture->map, &options);
    assert(lower_result.error_count == 0u);
    function = find_function(&fixture->hir, function_name);
    assert(function != NULL
        && function->data.function_item.body != CM_HIR_BODY_NONE);
    fixture->body = function->data.function_item.body;
}

static void fixture_init(TestFixture *fixture, const char *source)
{
    fixture_init_named(fixture, source, "main");
}

static void fixture_destroy(TestFixture *fixture)
{
    cm_hir_module_map_destroy(&fixture->map);
    cm_hir_context_destroy(&fixture->hir);
    cm_import_resolver_destroy(&fixture->imports);
    cm_module_graph_destroy(&fixture->graph);
    cm_source_set_destroy(&fixture->sources);
}

static CmHirBodyLowerResult lower_fixture_body(TestFixture *fixture)
{
    return cm_hir_lower_body(&fixture->hir, fixture->body,
        &fixture->graph, fixture->graph_result.revision, &fixture->imports,
        &fixture->map);
}

static CmHirLocalBodiesResult lower_fixture_local_bodies(
    TestFixture *fixture)
{
    const CmHirCrate *crate_value;

    crate_value = cm_hir_get_crate(&fixture->hir, 1u);
    assert(crate_value != NULL);
    return cm_hir_lower_local_bodies(&fixture->hir, 1u, &fixture->graph,
        fixture->graph_result.revision, &fixture->imports, &fixture->map);
}

typedef struct TestHirSnapshot {
    uint64_t semantic_generation;
    uint64_t rewind_generation;
    size_t crate_count;
    size_t module_count;
    size_t item_count;
    size_t body_count;
    size_t expression_count;
    size_t type_count;
    size_t generic_parameter_count;
    size_t definition_count;
    size_t prebound_associated_type_count;
    size_t storage_block_count;
    size_t storage_bytes_used;
    size_t storage_capacity;
    size_t string_count;
    size_t string_block_count;
    size_t string_bytes_used;
    size_t string_capacity;
    size_t string_map_count;
    CmHirBody *bodies;
    char *dump;
} TestHirSnapshot;

static void hir_snapshot_take(const CmHirContext *hir,
    TestHirSnapshot *snapshot)
{
    FILE *stream;
    size_t body_bytes;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->semantic_generation = hir->semantic_generation;
    snapshot->rewind_generation = hir->rewind_generation;
    snapshot->crate_count = hir->crates.len;
    snapshot->module_count = hir->modules.len;
    snapshot->item_count = hir->items.len;
    snapshot->body_count = hir->bodies.len;
    snapshot->expression_count = hir->expressions.len;
    snapshot->type_count = hir->types.len;
    snapshot->generic_parameter_count = hir->generic_parameters.len;
    snapshot->definition_count = hir->definitions.len;
    snapshot->prebound_associated_type_count =
        hir->prebound_associated_types.len;
    snapshot->storage_block_count = cm_arena_block_count(&hir->storage);
    snapshot->storage_bytes_used = cm_arena_bytes_used(&hir->storage);
    snapshot->storage_capacity = cm_arena_capacity(&hir->storage);
    snapshot->string_count = cm_interner_length(&hir->strings);
    snapshot->string_block_count = cm_arena_block_count(&hir->strings.strings);
    snapshot->string_bytes_used = cm_arena_bytes_used(&hir->strings.strings);
    snapshot->string_capacity = cm_arena_capacity(&hir->strings.strings);
    snapshot->string_map_count = cm_map_length(&hir->strings.by_text);
    body_bytes = snapshot->body_count * sizeof(*snapshot->bodies);
    if (body_bytes != 0u) {
        snapshot->bodies = (CmHirBody *)malloc(body_bytes);
        assert(snapshot->bodies != NULL);
        memcpy(snapshot->bodies, hir->bodies.data, body_bytes);
    }
    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, hir) == 0);
    snapshot->dump = read_dump(stream);
    assert(fclose(stream) == 0);
}

static void hir_snapshot_assert_unchanged(const CmHirContext *hir,
    const TestHirSnapshot *snapshot)
{
    FILE *stream;
    char *dump;
    size_t body_bytes;

    assert(hir->crates.len == snapshot->crate_count
        && hir->modules.len == snapshot->module_count
        && hir->items.len == snapshot->item_count
        && hir->bodies.len == snapshot->body_count
        && hir->expressions.len == snapshot->expression_count
        && hir->types.len == snapshot->type_count
        && hir->generic_parameters.len == snapshot->generic_parameter_count
        && hir->definitions.len == snapshot->definition_count
        && hir->prebound_associated_types.len
            == snapshot->prebound_associated_type_count
        && cm_arena_block_count(&hir->storage)
            == snapshot->storage_block_count
        && cm_arena_bytes_used(&hir->storage)
            == snapshot->storage_bytes_used
        && cm_arena_capacity(&hir->storage) == snapshot->storage_capacity
        && cm_interner_length(&hir->strings) == snapshot->string_count
        && cm_arena_block_count(&hir->strings.strings)
            == snapshot->string_block_count
        && cm_arena_bytes_used(&hir->strings.strings)
            == snapshot->string_bytes_used
        && cm_arena_capacity(&hir->strings.strings)
            == snapshot->string_capacity
        && cm_map_length(&hir->strings.by_text) == snapshot->string_map_count
        && hir->storage.active_marks.len == 0u
        && hir->strings.strings.active_marks.len == 0u);
    body_bytes = snapshot->body_count * sizeof(*snapshot->bodies);
    assert(body_bytes == 0u
        || memcmp(hir->bodies.data, snapshot->bodies, body_bytes) == 0);
    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, hir) == 0);
    dump = read_dump(stream);
    assert(strcmp(dump, snapshot->dump) == 0);
    free(dump);
    assert(fclose(stream) == 0);
}

static void hir_snapshot_destroy(TestHirSnapshot *snapshot)
{
    free(snapshot->dump);
    free(snapshot->bodies);
    memset(snapshot, 0, sizeof(*snapshot));
}

static void test_all_local_bodies_transaction(void)
{
    TestFixture fixture;
    CmHirLocalBodiesResult result;
    const CmHirItem *first;
    const CmHirItem *second;
    TestHirSnapshot snapshot;

    fixture_init_named(&fixture,
        "fn first() -> u32 { let inferred = 7; inferred } "
        "fn second() -> u32 { let mismatch = 9i32; mismatch }", "first");
    first = find_function(&fixture.hir, "first");
    second = find_function(&fixture.hir, "second");
    assert(first != NULL && second != NULL);
    hir_snapshot_take(&fixture.hir, &snapshot);
    result = lower_fixture_local_bodies(&fixture);
    assert(result.status == CM_HIR_LOCAL_BODIES_BODY_FAILURE
        && result.body == second->data.function_item.body
        && result.body_result.status == CM_HIR_BODY_LOWER_TYPE_MISMATCH
        && cm_hir_get_body(&fixture.hir,
            first->data.function_item.body)->state == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&fixture.hir,
            second->data.function_item.body)->state == CM_HIR_BODY_UNLOWERED);
    hir_snapshot_assert_unchanged(&fixture.hir, &snapshot);
    hir_snapshot_destroy(&snapshot);
    fixture_destroy(&fixture);

    fixture_init_named(&fixture,
        "fn first() -> i32 { 7 } fn second() -> i32 { 9 }", "first");
    result = lower_fixture_local_bodies(&fixture);
    assert(result.status == CM_HIR_LOCAL_BODIES_OK
        && cm_hir_get_body(&fixture.hir,
            find_function(&fixture.hir, "first")->data.function_item.body)
                ->state == CM_HIR_BODY_TYPED
        && cm_hir_get_body(&fixture.hir,
            find_function(&fixture.hir, "second")->data.function_item.body)
                ->state == CM_HIR_BODY_TYPED);
    fixture_destroy(&fixture);
}

static void test_free_value_body_owners(void)
{
    TestFixture fixture;
    CmHirLocalBodiesResult result;
    const CmHirItem *constant;
    const CmHirItem *braced;
    const CmHirItem *static_item;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *tail;

    fixture_init(&fixture,
        "fn ordinary(value: u32) -> u32 { value } "
        "const DIRECT: u32 = ordinary(4u32); "
        "const BRACED: u32 = { 2u32 }; "
        "static SLOT: i32 = 3i32; "
        "fn main() -> u32 { 0u32 }");
    constant = find_value(&fixture.hir, CM_HIR_ITEM_CONST, "DIRECT");
    braced = find_value(&fixture.hir, CM_HIR_ITEM_CONST, "BRACED");
    static_item = find_value(&fixture.hir, CM_HIR_ITEM_STATIC, "SLOT");
    assert(constant != NULL && braced != NULL && static_item != NULL
        && cm_hir_body_value_owner_kind(&fixture.hir, constant)
            == CM_HIR_BODY_VALUE_OWNER_FREE_CONST
        && cm_hir_body_value_owner_kind(&fixture.hir, braced)
            == CM_HIR_BODY_VALUE_OWNER_FREE_CONST
        && cm_hir_body_value_owner_kind(&fixture.hir, static_item)
            == CM_HIR_BODY_VALUE_OWNER_FREE_STATIC);
    result = lower_fixture_local_bodies(&fixture);
    assert(result.status == CM_HIR_LOCAL_BODIES_OK);

    body = cm_hir_get_body(&fixture.hir,
        constant->data.value_item.body);
    root = body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->local_count == 0u && body->parameter_count == 0u
        && root != NULL && root->kind == CM_HIR_EXPR_BLOCK
        && root->data.block.statement_count == 0u
        && tail != NULL && tail->kind == CM_HIR_EXPR_CALL);

    body = cm_hir_get_body(&fixture.hir, braced->data.value_item.body);
    root = body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && root != NULL && root->kind == CM_HIR_EXPR_BLOCK
        && root->data.block.statement_count == 0u
        && tail != NULL && tail->kind == CM_HIR_EXPR_INTEGER
        && tail->data.integer.low_bits == UINT64_C(2));

    body = cm_hir_get_body(&fixture.hir,
        static_item->data.value_item.body);
    root = body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && root != NULL && root->kind == CM_HIR_EXPR_BLOCK
        && tail != NULL && tail->kind == CM_HIR_EXPR_INTEGER
        && tail->data.integer.low_bits == UINT64_C(3));
    fixture_destroy(&fixture);
}

static void test_value_body_atomic_rollback_and_owner_rejection(void)
{
    TestFixture fixture;
    CmHirLocalBodiesResult result;
    const CmHirItem *constant;
    const CmHirItem *static_item;
    const CmHirItem *trait_default;
    CmHirItem *mutable_constant;
    TestHirSnapshot snapshot;

    fixture_init(&fixture,
        "fn main() -> i32 { 1i32 } "
        "const BAD: i32 = 2u32; static SLOT: i32 = 3i32;");
    constant = find_value(&fixture.hir, CM_HIR_ITEM_CONST, "BAD");
    static_item = find_value(&fixture.hir, CM_HIR_ITEM_STATIC, "SLOT");
    assert(constant != NULL && static_item != NULL);
    hir_snapshot_take(&fixture.hir, &snapshot);
    result = lower_fixture_local_bodies(&fixture);
    assert(result.status == CM_HIR_LOCAL_BODIES_BODY_FAILURE
        && result.body == constant->data.value_item.body
        && result.body_result.status == CM_HIR_BODY_LOWER_INVALID_LITERAL
        && cm_hir_get_body(&fixture.hir, fixture.body)->state
            == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&fixture.hir,
            constant->data.value_item.body)->state
                == CM_HIR_BODY_UNLOWERED
        && cm_hir_get_body(&fixture.hir,
            static_item->data.value_item.body)->state
                == CM_HIR_BODY_UNLOWERED);
    hir_snapshot_assert_unchanged(&fixture.hir, &snapshot);
    hir_snapshot_destroy(&snapshot);
    fixture_destroy(&fixture);

    fixture_init(&fixture,
        "trait HasValue { const VALUE: u32 = 1u32; } "
        "fn main() -> u32 { 0u32 }");
    trait_default = find_value(&fixture.hir, CM_HIR_ITEM_CONST, "VALUE");
    assert(trait_default != NULL
        && cm_hir_body_value_owner_kind(&fixture.hir, trait_default)
            == CM_HIR_BODY_VALUE_OWNER_UNSUPPORTED);
    result = lower_fixture_local_bodies(&fixture);
    assert(result.status == CM_HIR_LOCAL_BODIES_UNSUPPORTED_OWNER
        && result.body == trait_default->data.value_item.body);
    fixture_destroy(&fixture);

    fixture_init(&fixture,
        "const VALUE: u32 = 1u32; fn main() -> u32 { 0u32 }");
    constant = find_value(&fixture.hir, CM_HIR_ITEM_CONST, "VALUE");
    assert(constant != NULL);
    mutable_constant = (CmHirItem *)constant;
    mutable_constant->generic_parameter_start = 1u;
    mutable_constant->generic_parameter_count = 1u;
    assert(cm_hir_body_value_owner_kind(&fixture.hir, constant)
        == CM_HIR_BODY_VALUE_OWNER_UNSUPPORTED);
    result = lower_fixture_local_bodies(&fixture);
    assert(result.status == CM_HIR_LOCAL_BODIES_UNSUPPORTED_OWNER
        && result.body == constant->data.value_item.body);
    fixture_destroy(&fixture);
}

static void test_value_block_rejections_are_transactional(void)
{
    static const char *sources[] = {
        "const BAD: u32 = { let value = 1u32; value }; "
            "fn main() -> u32 { 0u32 }",
        "const BAD: u32 = unsafe { 1u32 }; "
            "fn main() -> u32 { 0u32 }",
        "const BAD: u32 = const { 1u32 }; "
            "fn main() -> u32 { 0u32 }"
    };
    size_t index;

    for (index = 0u; index < sizeof(sources) / sizeof(sources[0]); ++index) {
        TestFixture fixture;
        CmHirLocalBodiesResult result;
        const CmHirItem *constant;
        TestHirSnapshot snapshot;

        fixture_init(&fixture, sources[index]);
        constant = find_value(&fixture.hir, CM_HIR_ITEM_CONST, "BAD");
        assert(constant != NULL);
        hir_snapshot_take(&fixture.hir, &snapshot);
        result = lower_fixture_local_bodies(&fixture);
        assert(result.status == CM_HIR_LOCAL_BODIES_BODY_FAILURE
            && result.body == constant->data.value_item.body
            && result.body_result.status
                == CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
        hir_snapshot_assert_unchanged(&fixture.hir, &snapshot);
        hir_snapshot_destroy(&snapshot);
        fixture_destroy(&fixture);
    }
}

static void test_closed_trait_default_bodies(void)
{
    static const char *unsupported_owners[] = {
        "trait T { fn value(self) -> u32 { 1u32 } } "
            "fn main() -> u32 { 0u32 }",
        "trait T { fn value() -> Self { loop {} } } "
            "fn main() -> u32 { 0u32 }",
        "trait T<X> { fn value(x: u32) -> u32 { x } } "
            "fn main() -> u32 { 0u32 }",
        "trait T { fn value<X>(x: u32) -> u32 { x } } "
            "fn main() -> u32 { 0u32 }",
        "trait Base {} trait T: Base { "
            "fn value(x: u32) -> u32 { x } } "
            "fn main() -> u32 { 0u32 }",
        "struct Wrap { value: u32 } trait T { "
            "fn value(x: Wrap) -> Wrap { x } } "
            "fn main() -> u32 { 0u32 }",
        "trait T { fn value(x: &'static u32) -> &'static u32 { x } } "
            "fn main() -> u32 { 0u32 }",
        "trait T { unsafe fn value(x: u32) -> u32 { x } } "
            "fn main() -> u32 { 0u32 }"
    };
    static const char *unsupported_expressions[] = {
        "fn helper(x: u32) -> u32 { x } "
            "trait T { fn value(x: u32) -> u32 { helper(x) } } "
            "fn main() -> u32 { 0u32 }",
        "trait T { fn required(x: u32) -> u32; "
            "fn value(x: u32) -> u32 { T::required(x) } } "
            "fn main() -> u32 { 0u32 }",
        "trait T { fn required(x: u32) -> u32; "
            "fn value(x: u32) -> u32 { <u32 as T>::required(x) } } "
            "fn main() -> u32 { 0u32 }",
        "struct Wrap { value: u32 } "
            "trait T { fn value(x: u32) -> u32 { Wrap { value: x }.value } } "
            "fn main() -> u32 { 0u32 }",
        "trait T { fn required(&self) -> u32; "
            "fn value(x: u32) -> u32 { x.required() } } "
            "fn main() -> u32 { 0u32 }"
    };
    TestFixture fixture;
    CmHirLocalBodiesResult result;
    const CmHirItem *method;
    const CmHirItem *trait_item;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *tail;
    CmHirTraitPredicate predicate;
    CmHirPredicateScope predicate_scope;
    CmHirOutlivesPredicate outlives;
    CmInternId rust_abi;
    TestHirSnapshot snapshot;
    size_t index;

    fixture_init(&fixture,
        "trait Closed { fn bump(value: u32) -> u32 { value + 1u32 } "
        "fn choose(value: u32) -> u32 { "
        "if value == 0u32 { 1u32 } else { value } } } "
        "fn main() -> u32 { 0u32 }");
    method = find_trait_function(&fixture.hir, "bump");
    assert(method != NULL
        && cm_hir_body_function_owner_kind(&fixture.hir, method)
            == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT);
    result = lower_fixture_local_bodies(&fixture);
    body = cm_hir_get_body(&fixture.hir,
        method->data.function_item.body);
    root = body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    assert(result.status == CM_HIR_LOCAL_BODIES_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->parameter_count == 1u && body->local_count == 1u
        && root != NULL && root->kind == CM_HIR_EXPR_BLOCK
        && tail != NULL && tail->kind == CM_HIR_EXPR_BINARY
        && cm_hir_body_function_owner_kind(&fixture.hir, method)
            == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT);
    fixture_destroy(&fixture);

    for (index = 0u;
         index < sizeof(unsupported_owners) / sizeof(unsupported_owners[0]);
         ++index) {
        fixture_init(&fixture, unsupported_owners[index]);
        method = find_trait_function(&fixture.hir, "value");
        assert(method != NULL
            && cm_hir_body_function_owner_kind(&fixture.hir, method)
                == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
        result = lower_fixture_local_bodies(&fixture);
        assert(result.status == CM_HIR_LOCAL_BODIES_UNSUPPORTED_OWNER
            && result.body == method->data.function_item.body);
        fixture_destroy(&fixture);
    }

    for (index = 0u; index < sizeof(unsupported_expressions)
            / sizeof(unsupported_expressions[0]); ++index) {
        fixture_init(&fixture, unsupported_expressions[index]);
        method = find_trait_function(&fixture.hir, "value");
        assert(method != NULL
            && cm_hir_body_function_owner_kind(&fixture.hir, method)
                == CM_HIR_BODY_FUNCTION_OWNER_TRAIT_DEFAULT);
        hir_snapshot_take(&fixture.hir, &snapshot);
        result = lower_fixture_local_bodies(&fixture);
        assert(result.status == CM_HIR_LOCAL_BODIES_BODY_FAILURE
            && result.body == method->data.function_item.body
            && result.body_result.status
                == CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
        hir_snapshot_assert_unchanged(&fixture.hir, &snapshot);
        hir_snapshot_destroy(&snapshot);
        fixture_destroy(&fixture);
    }

    fixture_init(&fixture,
        "trait T { fn value(x: u32) -> u32 { x } } "
        "fn main() -> u32 { 0u32 }");
    method = find_trait_function(&fixture.hir, "value");
    trait_item = find_trait(&fixture.hir, "T");
    assert(method != NULL && trait_item != NULL);
    ((CmHirItem *)trait_item)->data.trait_item.is_auto = 1;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    ((CmHirItem *)trait_item)->data.trait_item.is_auto = 0;
    memset(&predicate, 0, sizeof(predicate));
    ((CmHirItem *)method)->predicates = &predicate;
    ((CmHirItem *)method)->predicate_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    ((CmHirItem *)method)->predicates = NULL;
    ((CmHirItem *)method)->predicate_count = 0u;
    ((CmHirItem *)trait_item)->predicates = &predicate;
    ((CmHirItem *)trait_item)->predicate_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    ((CmHirItem *)trait_item)->predicates = NULL;
    ((CmHirItem *)trait_item)->predicate_count = 0u;
    memset(&predicate_scope, 0, sizeof(predicate_scope));
    ((CmHirItem *)method)->predicate_scopes = &predicate_scope;
    ((CmHirItem *)method)->predicate_scope_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    ((CmHirItem *)method)->predicate_scopes = NULL;
    ((CmHirItem *)method)->predicate_scope_count = 0u;
    memset(&outlives, 0, sizeof(outlives));
    ((CmHirItem *)trait_item)->outlives_predicates = &outlives;
    ((CmHirItem *)trait_item)->outlives_predicate_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    ((CmHirItem *)trait_item)->outlives_predicates = NULL;
    ((CmHirItem *)trait_item)->outlives_predicate_count = 0u;
    ((CmHirItem *)method)->data.function_item.signature.receiver =
        CM_HIR_RECEIVER_VALUE;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    ((CmHirItem *)method)->data.function_item.signature.receiver =
        CM_HIR_RECEIVER_NONE;
    ((CmHirItem *)method)->data.function_item.signature.is_const = 1;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    ((CmHirItem *)method)->data.function_item.signature.is_const = 0;
    ((CmHirItem *)method)->data.function_item.signature.is_async = 1;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    ((CmHirItem *)method)->data.function_item.signature.is_async = 0;
    rust_abi = method->data.function_item.signature.abi;
    ((CmHirItem *)method)->data.function_item.signature.abi =
        cm_hir_intern(&fixture.hir, "C");
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    ((CmHirItem *)method)->data.function_item.signature.abi = rust_abi;
    fixture_destroy(&fixture);
}

static void test_generic_impl_method_body_owner(void)
{
    static const char accepted_source[] =
        "trait Echo { type Value; "
        "fn echo(value: Self::Value) -> Self::Value; } "
        "struct Wrap<T> { value: T } "
        "impl<T> Echo for Wrap<T> { "
        "type Value = T; "
        "fn echo(value: T) -> T { value } } "
        "fn main() -> u32 { 0u32 }";
    TestFixture fixture;
    CmHirLocalBodiesResult result;
    const CmHirItem *method;
    const CmHirItem *impl_item;
    const CmHirItem *trait_item;
    const CmHirItem *wrap_item;
    const CmHirDefinition *impl_definition;
    const CmHirGenericParam *parameter;
    const CmHirGenericParam *wrap_parameter;
    const CmHirBody *body;
    const CmHirType *impl_self_type;
    const CmHirType *return_type;
    const CmHirType *local_type;
    const CmHirType *tail_type;
    const CmHirExpr *root;
    const CmHirExpr *tail;
    CmHirItem *mutable_method;
    CmHirItem *mutable_impl;
    CmHirItem *mutable_wrap;
    CmHirGenericParam *mutable_parameter;
    CmHirGenericParam *mutable_wrap_parameter;
    CmHirNamedType *mutable_self_named;
    CmHirGenericArg saved_self_argument;
    CmHirDefId saved_definition;
    uint32_t saved_parameter_count;
    CmHirDefId saved_parameter_owner;
    CmHirGenericParamKind saved_parameter_kind;
    uint32_t saved_parameter_index;
    CmHirPredicateScope predicate_scope;
    CmHirTraitPredicate predicate;
    CmHirOutlivesPredicate outlives;

    fixture_init(&fixture, accepted_source);
    method = find_impl_function(&fixture.hir, "echo");
    impl_definition = method == NULL ? NULL
        : cm_hir_lookup_definition(&fixture.hir,
            method->parent_definition);
    impl_item = impl_definition == NULL
            || impl_definition->kind != CM_HIR_DEFINITION_ITEM
            || impl_definition->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(&fixture.hir,
            impl_definition->entity.item_id);
    trait_item = find_trait(&fixture.hir, "Echo");
    wrap_item = find_struct(&fixture.hir, "Wrap");
    parameter = impl_item == NULL ? NULL
        : cm_hir_get_generic_param(&fixture.hir,
            impl_item->generic_parameter_start);
    wrap_parameter = wrap_item == NULL ? NULL
        : cm_hir_get_generic_param(&fixture.hir,
            wrap_item->generic_parameter_start);
    impl_self_type = impl_item == NULL ? NULL
        : cm_hir_get_type(&fixture.hir,
            impl_item->data.impl_item.self_type);
    body = method == NULL ? NULL : cm_hir_get_body(&fixture.hir,
        method->data.function_item.body);
    return_type = method == NULL ? NULL : cm_hir_get_type(&fixture.hir,
        method->data.function_item.signature.return_type);
    local_type = body == NULL || body->local_count != 1u ? NULL
        : cm_hir_get_type(&fixture.hir, body->locals[0].type);
    assert(method != NULL && impl_item != NULL && trait_item != NULL
        && wrap_item != NULL && wrap_parameter != NULL
        && impl_self_type != NULL
        && impl_self_type->kind == CM_HIR_TYPE_ADT_KIND
        && impl_item->generic_parameter_count == 1u
        && method->generic_parameter_count == 0u
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 0u
        && cm_hir_def_id_equal(parameter->owner, impl_item->definition)
        && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && return_type->data.parameter_type.parameter
            == impl_item->generic_parameter_start
        && local_type != NULL
        && local_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && local_type->data.parameter_type.parameter
            == impl_item->generic_parameter_start
        && cm_hir_body_function_owner_kind(&fixture.hir, method)
            == CM_HIR_BODY_FUNCTION_OWNER_TYPE_GENERIC_TRAIT_IMPL_METHOD);

    mutable_method = (CmHirItem *)method;
    mutable_impl = (CmHirItem *)impl_item;
    mutable_wrap = (CmHirItem *)wrap_item;
    mutable_parameter = (CmHirGenericParam *)parameter;
    mutable_wrap_parameter = (CmHirGenericParam *)wrap_parameter;
    mutable_self_named = &((CmHirType *)impl_self_type)->data.named_type;

    saved_definition = mutable_self_named->definition;
    mutable_self_named->definition = method->definition;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_self_named->definition = trait_item->definition;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_self_named->definition = saved_definition;

    saved_definition = mutable_impl->data.impl_item.trait_type.definition;
    mutable_impl->data.impl_item.trait_type.definition =
        wrap_item->definition;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_impl->data.impl_item.trait_type.definition = saved_definition;

    saved_parameter_count = mutable_wrap->generic_parameter_count;
    mutable_wrap->generic_parameter_count = 0u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_wrap->generic_parameter_count = saved_parameter_count;

    assert(mutable_self_named->argument_count == 1u);
    saved_self_argument = mutable_self_named->arguments[0];
    mutable_self_named->arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    mutable_self_named->arguments[0].data.lifetime.kind =
        CM_HIR_REGION_STATIC;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_self_named->arguments[0] = saved_self_argument;

    saved_parameter_owner = mutable_wrap_parameter->owner;
    mutable_wrap_parameter->owner = mutable_impl->definition;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_wrap_parameter->owner = saved_parameter_owner;
    saved_parameter_index = mutable_wrap_parameter->index;
    mutable_wrap_parameter->index = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_wrap_parameter->index = saved_parameter_index;
    saved_parameter_kind = mutable_wrap_parameter->kind;
    mutable_wrap_parameter->kind = CM_HIR_GENERIC_CONST;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_wrap_parameter->kind = saved_parameter_kind;

    mutable_parameter->kind = CM_HIR_GENERIC_LIFETIME;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_parameter->kind = CM_HIR_GENERIC_CONST;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_parameter->kind = CM_HIR_GENERIC_TYPE;
    mutable_parameter->has_default = 1;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_parameter->has_default = 0;
    mutable_parameter->is_relaxed_sized = 1;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_parameter->is_relaxed_sized = 0;

    mutable_method->generic_parameter_start =
        mutable_impl->generic_parameter_start;
    mutable_method->generic_parameter_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_method->generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    mutable_method->generic_parameter_count = 0u;

    memset(&predicate_scope, 0, sizeof(predicate_scope));
    mutable_impl->predicate_scopes = &predicate_scope;
    mutable_impl->predicate_scope_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_impl->predicate_scopes = NULL;
    mutable_impl->predicate_scope_count = 0u;
    mutable_method->predicate_scopes = &predicate_scope;
    mutable_method->predicate_scope_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_method->predicate_scopes = NULL;
    mutable_method->predicate_scope_count = 0u;

    memset(&predicate, 0, sizeof(predicate));
    mutable_impl->predicates = &predicate;
    mutable_impl->predicate_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_impl->predicates = NULL;
    mutable_impl->predicate_count = 0u;
    mutable_method->predicates = &predicate;
    mutable_method->predicate_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_method->predicates = NULL;
    mutable_method->predicate_count = 0u;

    memset(&outlives, 0, sizeof(outlives));
    mutable_impl->outlives_predicates = &outlives;
    mutable_impl->outlives_predicate_count = 1u;
    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_UNSUPPORTED);
    mutable_impl->outlives_predicates = NULL;
    mutable_impl->outlives_predicate_count = 0u;

    assert(cm_hir_body_function_owner_kind(&fixture.hir, method)
        == CM_HIR_BODY_FUNCTION_OWNER_TYPE_GENERIC_TRAIT_IMPL_METHOD);
    result = lower_fixture_local_bodies(&fixture);
    body = cm_hir_get_body(&fixture.hir,
        method->data.function_item.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    return_type = method == NULL ? NULL : cm_hir_get_type(&fixture.hir,
        method->data.function_item.signature.return_type);
    local_type = body == NULL || body->local_count != 1u ? NULL
        : cm_hir_get_type(&fixture.hir, body->locals[0].type);
    tail_type = tail == NULL ? NULL
        : cm_hir_get_type(&fixture.hir, tail->type);
    assert(result.status == CM_HIR_LOCAL_BODIES_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->expected_type
            == method->data.function_item.signature.return_type
        && tail != NULL && tail->kind == CM_HIR_EXPR_LOCAL
        && tail->type == body->expected_type
        && tail->data.local.local_index == 0u
        && return_type != NULL
        && return_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && return_type->data.parameter_type.parameter
            == impl_item->generic_parameter_start
        && local_type != NULL
        && local_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && local_type->data.parameter_type.parameter
            == impl_item->generic_parameter_start
        && tail_type != NULL
        && tail_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && tail_type->data.parameter_type.parameter
            == impl_item->generic_parameter_start
        && cm_hir_body_function_owner_kind(&fixture.hir, method)
            == CM_HIR_BODY_FUNCTION_OWNER_TYPE_GENERIC_TRAIT_IMPL_METHOD);
    fixture_destroy(&fixture);
}

static char *read_dump(FILE *stream)
{
    long length;
    char *text;

    assert(fflush(stream) == 0);
    assert(fseek(stream, 0L, SEEK_END) == 0);
    length = ftell(stream);
    assert(length >= 0L);
    assert(fseek(stream, 0L, SEEK_SET) == 0);
    text = (char *)malloc((size_t)length + 1u);
    assert(text != NULL);
    assert(fread(text, 1u, (size_t)length, stream) == (size_t)length);
    text[(size_t)length] = '\0';
    return text;
}

static void test_exact_i32_body(void)
{
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *literal;
    CmHirExpr invalid;
    CmHirExprId invalid_id;
    CmHirType unit;
    CmHirTypeId unit_id;
    size_t expression_count;
    size_t type_count;
    FILE *stream;
    char *dump;
    char body_record[96];

    fixture_init(&fixture,
        "pub extern \"C\" fn main() -> i32 { 7 }");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->source == fixture.source && body->source_expression_id != 0u
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u);
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && result.body == fixture.body
        && result.root_expression != CM_HIR_EXPR_NONE
        && result.hir_status == CM_HIR_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->source == fixture.source && body->source_expression_id != 0u
        && body->root_expression == result.root_expression);
    root = cm_hir_get_expr(&fixture.hir, body->root_expression);
    assert(root != NULL && root->kind == CM_HIR_EXPR_BLOCK
        && root->type == body->expected_type
        && root->span.source == body->source);
    literal = cm_hir_get_expr(&fixture.hir,
        root->data.block.tail_expression);
    assert(literal != NULL && literal->kind == CM_HIR_EXPR_INTEGER
        && literal->type == root->type
        && literal->data.integer.low_bits == 7u
        && literal->data.integer.high_bits == 0u
        && literal->span.start >= root->span.start
        && literal->span.end <= root->span.end
        && cm_hir_get_expr(&fixture.hir, CM_HIR_EXPR_NONE) == NULL);

    expression_count = fixture.hir.expressions.len;
    type_count = fixture.hir.types.len;
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_BODY
        && fixture.hir.expressions.len == expression_count
        && fixture.hir.types.len == type_count
        && cm_hir_get_body(&fixture.hir, fixture.body)->root_expression
            == body->root_expression);

    memset(&unit, 0, sizeof(unit));
    unit.kind = CM_HIR_TYPE_UNIT_KIND;
    unit.span = body->span;
    assert(cm_hir_add_type(&fixture.hir, &unit, &unit_id) == CM_HIR_OK);
    memset(&invalid, 0, sizeof(invalid));
    invalid.kind = CM_HIR_EXPR_INTEGER;
    invalid.type = unit_id;
    invalid.span = body->span;
    invalid_id = 99u;
    assert(cm_hir_add_expr(&fixture.hir, &invalid, &invalid_id)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(invalid_id == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == expression_count);
    invalid.kind = CM_HIR_EXPR_BLOCK;
    invalid.type = body->expected_type;
    invalid.data.block.tail_expression = CM_HIR_EXPR_NONE;
    assert(cm_hir_add_expr(&fixture.hir, &invalid, &invalid_id)
        == CM_HIR_INVARIANT_VIOLATION
        && fixture.hir.expressions.len == expression_count);

    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, &fixture.hir) == 0);
    dump = read_dump(stream);
    assert(strstr(dump,
        "expr#1 integer type=ty#1 bits=0x0000000000000000:"
        "0000000000000007") != NULL);
    assert(strstr(dump,
        "expr#2 block type=ty#1 statements=[] tail=expr#1") != NULL);
    assert(snprintf(body_record, sizeof(body_record),
        "state=typed expected=ty#1 locals=0 params=0 source-expr=%u:",
        (unsigned int)fixture.source) > 0);
    assert(strstr(dump, body_record) != NULL);
    free(dump);
    assert(fclose(stream) == 0);
    fixture_destroy(&fixture);
}

static void test_source_backed_named_aggregate_body(void)
{
    static const char source[] =
        "struct Pair { first: u32, second: i32 }\n"
        "fn make(first: u32) -> Pair { Pair { second: 7, first } }\n";
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirItem *pair;
    const CmHirExpr *root;
    const CmHirExpr *aggregate;
    const CmHirExpr *second;
    const CmHirExpr *first;

    fixture_init_named(&fixture, source, "make");
    pair = find_struct(&fixture.hir, "Pair");
    assert(pair != NULL && pair->data.aggregate_item.field_count == 2u);
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && result.root_expression != CM_HIR_EXPR_NONE);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    aggregate = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && aggregate != NULL && aggregate->kind == CM_HIR_EXPR_AGGREGATE
        && cm_hir_def_id_equal(aggregate->data.aggregate.definition,
            pair->definition)
        && aggregate->type == body->expected_type
        && aggregate->data.aggregate.field_count == 2u
        && aggregate->data.aggregate.fields != NULL
        && aggregate->data.aggregate.owned_storage
            == aggregate->data.aggregate.fields
        && aggregate->data.aggregate.fields[0].field_index == 1u
        && aggregate->data.aggregate.fields[1].field_index == 0u
        && source_span_is(source,
            aggregate->data.aggregate.fields[0].span, "second: 7")
        && source_span_is(source,
            aggregate->data.aggregate.fields[1].span, "first"));
    second = cm_hir_get_expr(&fixture.hir,
        aggregate->data.aggregate.fields[0].value);
    first = cm_hir_get_expr(&fixture.hir,
        aggregate->data.aggregate.fields[1].value);
    assert(second != NULL && second->kind == CM_HIR_EXPR_INTEGER
        && second->type == pair->data.aggregate_item.fields[1].type
        && second->owner_body == fixture.body
        && first != NULL && first->kind == CM_HIR_EXPR_LOCAL
        && first->type == pair->data.aggregate_item.fields[0].type
        && first->owner_body == fixture.body
        && first->data.local.local_index == 0u);
    fixture_destroy(&fixture);
}

static void test_nested_and_empty_named_aggregate_bodies(void)
{
    static const char nested_source[] =
        "struct Inner { value: u32 }\n"
        "struct Outer { inner: Inner, tail: i32 }\n"
        "fn make(value: u32) -> Outer {\n"
        "    Outer { tail: 3i32, inner: Inner { value } }\n"
        "}\n";
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *outer;
    const CmHirExpr *inner;

    fixture_init_named(&fixture, nested_source, "make");
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    outer = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    inner = outer == NULL || outer->kind != CM_HIR_EXPR_AGGREGATE
            || outer->data.aggregate.field_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            outer->data.aggregate.fields[1].value);
    assert(outer != NULL && outer->kind == CM_HIR_EXPR_AGGREGATE
        && outer->data.aggregate.fields[0].field_index == 1u
        && outer->data.aggregate.fields[1].field_index == 0u
        && outer->data.aggregate.owned_storage
            == outer->data.aggregate.fields
        && inner != NULL && inner->kind == CM_HIR_EXPR_AGGREGATE
        && inner->data.aggregate.field_count == 1u
        && inner->data.aggregate.fields != NULL
        && inner->data.aggregate.owned_storage == NULL
        && inner->data.aggregate.fields[0].field_index == 0u
        && inner->data.aggregate.fields
            == outer->data.aggregate.fields + 2u);
    fixture_destroy(&fixture);

    fixture_init_named(&fixture,
        "struct Empty {} fn make() -> Empty { Empty {} }", "make");
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    outer = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    assert(outer != NULL && outer->kind == CM_HIR_EXPR_AGGREGATE
        && outer->data.aggregate.field_count == 0u
        && outer->data.aggregate.fields == NULL
        && outer->data.aggregate.owned_storage == NULL);
    fixture_destroy(&fixture);
}

static void test_imported_alias_named_aggregate_body(void)
{
    static const char source[] =
        "mod defs {\n"
        "    pub struct Pair { pub left: u32, pub right: i32 }\n"
        "}\n"
        "use crate::defs::Pair as Renamed;\n"
        "fn make(left: u32) -> Renamed {\n"
        "    Renamed { right: 7i32, left }\n"
        "}\n";
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirItem *pair;
    const CmHirExpr *root;
    const CmHirExpr *aggregate;

    fixture_init_named(&fixture, source, "make");
    pair = find_struct(&fixture.hir, "Pair");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    aggregate = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    assert(result.status == CM_HIR_BODY_LOWER_OK && pair != NULL
        && aggregate != NULL && aggregate->kind == CM_HIR_EXPR_AGGREGATE
        && cm_hir_def_id_equal(aggregate->data.aggregate.definition,
            pair->definition)
        && aggregate->data.aggregate.fields[0].field_index == 1u
        && aggregate->data.aggregate.fields[1].field_index == 0u);
    fixture_destroy(&fixture);
}

static void test_imported_parameter_named_field_projection(void)
{
    static const char source[] =
        "mod defs {\n"
        "    pub struct Pair { pub first: u32, pub second: i32 }\n"
        "}\n"
        "use crate::defs::Pair as Renamed;\n"
        "fn read(pair: Renamed) -> i32 { pair.second }\n";
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirItem *pair;
    const CmHirExpr *root;
    const CmHirExpr *field;
    const CmHirExpr *base;
    const CmHirType *field_type;
    const CmHirType *declared_type;

    fixture_init_named(&fixture, source, "read");
    pair = find_struct(&fixture.hir, "Pair");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    field = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    base = field == NULL || field->kind != CM_HIR_EXPR_FIELD ? NULL
        : cm_hir_get_expr(&fixture.hir, field->data.field.base);
    field_type = field == NULL ? NULL : cm_hir_get_type(&fixture.hir,
        field->type);
    declared_type = pair == NULL ? NULL : cm_hir_get_type(&fixture.hir,
        pair->data.aggregate_item.fields[1].type);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->local_count == 1u
        && pair != NULL && pair->data.aggregate_item.field_count == 2u);
    assert(field != NULL && field->kind == CM_HIR_EXPR_FIELD
        && field->owner_body == fixture.body);
    assert(field->type == pair->data.aggregate_item.fields[1].type
        && field_type != NULL && declared_type != NULL
        && field_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && declared_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && field_type->data.integer_type.kind == CM_HIR_INT_I32
        && declared_type->data.integer_type.kind == CM_HIR_INT_I32);
    assert(cm_hir_def_id_equal(field->data.field.definition,
            pair->definition)
        && field->data.field.field_index == 1u);
    assert(field->span.source == fixture.source
        && source_span_is(source, field->span, "pair.second"));
    assert(base != NULL && base->kind == CM_HIR_EXPR_LOCAL
        && base->owner_body == fixture.body
        && base->type == body->locals[0].type
        && base->span.source == fixture.source
        && source_span_is(source, base->span, "pair")
        && base->data.local.local_index == 0u);
    fixture_destroy(&fixture);
}

static void test_constructed_nested_named_field_projection(void)
{
    static const char source[] =
        "struct Inner { value: u32 }\n"
        "struct Outer { inner: Inner, tail: i32 }\n"
        "fn read(value: u32) -> u32 {\n"
        "    Outer { tail: 3i32, inner: Inner { value } }.inner.value\n"
        "}\n";
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirItem *inner_item;
    const CmHirItem *outer_item;
    const CmHirExpr *root;
    const CmHirExpr *value_field;
    const CmHirExpr *inner_field;
    const CmHirExpr *outer;
    const CmHirExpr *inner;

    fixture_init_named(&fixture, source, "read");
    inner_item = find_struct(&fixture.hir, "Inner");
    outer_item = find_struct(&fixture.hir, "Outer");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    value_field = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    inner_field = value_field == NULL
            || value_field->kind != CM_HIR_EXPR_FIELD
        ? NULL : cm_hir_get_expr(&fixture.hir,
            value_field->data.field.base);
    outer = inner_field == NULL || inner_field->kind != CM_HIR_EXPR_FIELD
        ? NULL : cm_hir_get_expr(&fixture.hir,
            inner_field->data.field.base);
    inner = outer == NULL || outer->kind != CM_HIR_EXPR_AGGREGATE
            || outer->data.aggregate.field_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            outer->data.aggregate.fields[1].value);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && inner_item != NULL && outer_item != NULL);
    assert(value_field != NULL
        && value_field->kind == CM_HIR_EXPR_FIELD
        && value_field->owner_body == fixture.body
        && value_field->type
            == inner_item->data.aggregate_item.fields[0].type
        && value_field->span.source == fixture.source
        && source_span_is(source, value_field->span,
            "Outer { tail: 3i32, inner: Inner { value } }.inner.value")
        && cm_hir_def_id_equal(value_field->data.field.definition,
            inner_item->definition)
        && value_field->data.field.field_index == 0u);
    assert(inner_field != NULL && inner_field->kind == CM_HIR_EXPR_FIELD
        && inner_field->owner_body == fixture.body
        && inner_field->type
            == outer_item->data.aggregate_item.fields[0].type
        && inner_field->span.source == fixture.source
        && source_span_is(source, inner_field->span,
            "Outer { tail: 3i32, inner: Inner { value } }.inner")
        && cm_hir_def_id_equal(inner_field->data.field.definition,
            outer_item->definition)
        && inner_field->data.field.field_index == 0u);
    assert(outer != NULL && outer->kind == CM_HIR_EXPR_AGGREGATE
        && outer->owner_body == fixture.body
        && cm_hir_def_id_equal(outer->data.aggregate.definition,
            outer_item->definition)
        && outer->data.aggregate.fields[0].field_index == 1u
        && outer->data.aggregate.fields[1].field_index == 0u
        && inner != NULL && inner->kind == CM_HIR_EXPR_AGGREGATE
        && inner->owner_body == fixture.body
        && cm_hir_def_id_equal(inner->data.aggregate.definition,
            inner_item->definition));
    fixture_destroy(&fixture);
}

static void expect_body_failure(const char *source,
    CmHirBodyLowerStatus expected)
{
    TestFixture fixture;
    TestHirSnapshot snapshot;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    size_t expression_count;
    size_t type_count;
    size_t arena_bytes;
    size_t string_count;
    const CmHirLocal *locals;
    uint32_t local_count;
    uint32_t source_expression;

    fixture_init(&fixture, source);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL);
    expression_count = fixture.hir.expressions.len;
    type_count = fixture.hir.types.len;
    arena_bytes = cm_arena_bytes_used(&fixture.hir.storage);
    string_count = cm_interner_length(&fixture.hir.strings);
    locals = body->locals;
    local_count = body->local_count;
    source_expression = body->source_expression_id;
    hir_snapshot_take(&fixture.hir, &snapshot);
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == expected
        && fixture.hir.semantic_generation
            == snapshot.semantic_generation
        && fixture.hir.rewind_generation == snapshot.rewind_generation
        && result.root_expression == CM_HIR_EXPR_NONE
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && body->source == fixture.source
        && body->source_expression_id == source_expression
        && body->locals == locals && body->local_count == local_count
        && fixture.hir.expressions.len == expression_count
        && fixture.hir.types.len == type_count
        && cm_arena_bytes_used(&fixture.hir.storage) == arena_bytes
        && cm_interner_length(&fixture.hir.strings) == string_count
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    hir_snapshot_assert_unchanged(&fixture.hir, &snapshot);
    hir_snapshot_destroy(&snapshot);
    fixture_destroy(&fixture);
}

static void test_rejections_are_transactional(void)
{
    TestFixture fixture;
    CmHirBodyLowerResult result;

    expect_body_failure("fn main() -> i32 {}",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure("fn main() -> i32 { 7u32 }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure("fn main() -> i32 { 2147483648 }",
        CM_HIR_BODY_LOWER_LITERAL_OUT_OF_RANGE);
    expect_body_failure("fn main() -> i32 { 2147483648i32 }",
        CM_HIR_BODY_LOWER_LITERAL_OUT_OF_RANGE);
    expect_body_failure("fn main() -> i32 { 1i32; 7i32 }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main() -> u32 { const LOCAL: u32 = 1u32; LOCAL }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main() -> u32 { fn helper() -> u32 { 1u32 } helper() }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main() -> u32 { union Local<T> { value: T } 0u32 }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main() -> u32 { macro_rules! local { () => { 0u32 }; } "
        "local!() }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure("fn main() -> u32 { const { 1u32 } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main() -> u32 { #[allow(dead_code)] 1u32 }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main() -> u32 { match 1u32 { "
        "value if let found = 1u32 => value, _ => 0u32 } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(num: u32) -> u32 { num.wrapping_add::<u32>(1u32) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(value: u32) -> u32 { value? }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(value: (u32, u32)) -> u32 { value.0 }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main() -> u32 { <u32 as Trait>::VALUE }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn pick(value: u32) -> u32 { value } "
        "fn main(value: u32) -> u32 { "
        "<Missing as MissingTrait>::pick(value) }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
    expect_body_failure("fn main() -> u32 { 7i32 }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure(
        "struct Pair { left: u32, right: i32 } "
        "fn main(left: u32) -> Pair { Pair { left } }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_body_failure(
        "struct Pair { left: u32, right: i32 } "
        "fn main(left: u32) -> Pair { Pair { left, left } }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_body_failure(
        "struct Pair { left: u32, right: i32 } "
        "fn main(left: u32) -> Pair { Pair { left, missing: 7i32 } }",
        CM_HIR_BODY_LOWER_INVALID_BODY);
    expect_body_failure(
        "struct Pair { left: u32, right: i32 } "
        "fn main(left: u32) -> Pair { Pair { left, right: 7u32 } }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure(
        "struct Pair { left: u32, right: i32 } "
        "fn main(left: u32, base: Pair) -> Pair { "
        "Pair { left, ..base } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "struct Pair { value: u32 } struct Other { value: u32 } "
        "fn main(value: u32) -> Pair { Other { value } }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_body_failure(
        "struct Generic<T> { value: T } "
        "fn main(value: u32) -> Generic<u32> { Generic { value } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE);
    expect_body_failure(
        "mod defs { pub struct Pair { pub left: u32, hidden: u32 } } "
        "fn main() -> defs::Pair { "
        "defs::Pair { left: 1u32, hidden: 2u32 } }",
        CM_HIR_BODY_LOWER_INVALID_BODY);

    fixture_init(&fixture, "fn main() -> i32 { 7i32 }");
    result = cm_hir_lower_body(&fixture.hir, fixture.body, &fixture.graph,
        fixture.graph_result.revision, NULL, &fixture.map);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_ARGUMENT
        && fixture.hir.expressions.len == 0u
        && cm_hir_get_body(&fixture.hir, fixture.body)->state
            == CM_HIR_BODY_UNLOWERED);
    {
        TestFixture other;

        fixture_init(&other, "fn main() -> i32 { 9i32 }");
        result = cm_hir_lower_body(&fixture.hir, fixture.body, &other.graph,
            other.graph_result.revision, &other.imports, &other.map);
        fixture_destroy(&other);
    }
    assert(result.status == CM_HIR_BODY_LOWER_SOURCE_MISMATCH
        && fixture.hir.expressions.len == 0u
        && cm_hir_get_body(&fixture.hir, fixture.body)->state
            == CM_HIR_BODY_UNLOWERED);
    fixture_destroy(&fixture);
}

static void test_graph_explicit_local_ufcs_call(void)
{
    static const char source[] =
        "trait Value { fn value(self) -> u32; } "
        "impl Value for u32 { fn value(self) -> u32 { self } } "
        "fn main(x: u32) -> u32 { <u32 as Value>::value(x) }";
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirExpr *block;
    const CmHirExpr *call;
    const CmHirExpr *argument;
    const CmHirItem *trait_item;
    const CmHirItem *declared;
    CmHirBodyLowerResult result;

    fixture_init(&fixture, source);
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    block = body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, body->root_expression);
    call = block == NULL || block->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            block->data.block.tail_expression);
    assert(call != NULL && call->kind == CM_HIR_EXPR_QUALIFIED_CALL
        && call->data.qualified_call.syntax
            == CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD
        && call->data.qualified_call.argument_count == 1u
        && call->data.qualified_call.arguments != NULL
        && call->data.qualified_call.receiver_argument == 0u
        && cm_hir_get_type(&fixture.hir,
            call->data.qualified_call.requested_self_type)->kind
                == CM_HIR_TYPE_INTEGER_KIND
        && cm_hir_get_type(&fixture.hir,
            call->data.qualified_call.requested_self_type)
                ->data.integer_type.kind == CM_HIR_INT_U32);
    trait_item = cm_hir_get_item(&fixture.hir,
        cm_hir_lookup_definition(&fixture.hir,
            call->data.qualified_call.requested_trait)->entity.item_id);
    declared = cm_hir_get_item(&fixture.hir,
        cm_hir_lookup_definition(&fixture.hir,
            call->data.qualified_call.declared_trait_callable)
                ->entity.item_id);
    argument = cm_hir_get_expr(&fixture.hir,
        call->data.qualified_call.arguments[0]);
    assert(trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
        && hir_name_is(&fixture.hir, trait_item->name, "Value")
        && declared != NULL && declared->kind == CM_HIR_ITEM_FUNCTION
        && hir_name_is(&fixture.hir, declared->name, "value")
        && cm_hir_def_id_equal(declared->parent_definition,
            trait_item->definition)
        && declared->data.function_item.body == CM_HIR_BODY_NONE
        && argument != NULL && argument->kind == CM_HIR_EXPR_LOCAL
        && argument->data.local.local_index == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_unresolved_local_method_call(void)
{
    static const char source[] =
        "mod traits { "
        "pub trait Imported { fn imported(self) -> u32; } "
        "pub trait Shadowed { fn shadowed(self) -> u32; } } "
        "use traits::*; "
        "use traits::Imported as _; "
        "trait Value { fn value(self, other: u32) -> u32; } "
        "struct Shadowed; "
        "fn main(x: u32) -> u32 { x.value(7u32) }";
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirExpr *block;
    const CmHirExpr *call;
    const CmHirExpr *receiver;
    const CmHirExpr *argument;
    const CmHirItem *value_trait;
    const CmHirItem *imported_trait;
    const CmHirItem *shadowed_trait;
    CmHirBodyLowerResult result;
    uint32_t index;
    int saw_value;
    int saw_imported;
    int saw_shadowed;

    fixture_init(&fixture, source);
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    block = body == NULL ? NULL
        : cm_hir_get_expr(&fixture.hir, body->root_expression);
    call = block == NULL || block->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            block->data.block.tail_expression);
    assert(call != NULL && call->kind == CM_HIR_EXPR_METHOD_CALL
        && call->data.method_call.syntax == CM_HIR_CALLABLE_DOT_METHOD
        && hir_name_is(&fixture.hir,
            call->data.method_call.method_name, "value")
        && call->data.method_call.argument_count == 1u
        && call->data.method_call.arguments != NULL);
    receiver = cm_hir_get_expr(&fixture.hir,
        call->data.method_call.receiver);
    assert(receiver != NULL && receiver->kind == CM_HIR_EXPR_LOCAL
        && receiver->data.local.local_index == 0u
        && receiver->owner_body == call->owner_body
        && receiver->span.start == call->span.start
        && receiver->span.end < call->span.end);
    argument = cm_hir_get_expr(&fixture.hir,
        call->data.method_call.arguments[0]);
    assert(argument != NULL && argument->kind == CM_HIR_EXPR_INTEGER
        && argument->data.integer.low_bits == 7u
        && argument->owner_body == call->owner_body
        && argument->span.start > receiver->span.end
        && argument->span.end < call->span.end);
    value_trait = find_trait(&fixture.hir, "Value");
    imported_trait = find_trait(&fixture.hir, "Imported");
    shadowed_trait = find_trait(&fixture.hir, "Shadowed");
    assert(value_trait != NULL && imported_trait != NULL
        && shadowed_trait != NULL);
    saw_value = 0;
    saw_imported = 0;
    saw_shadowed = 0;
    for (index = 0u; index < call->data.method_call.in_scope_trait_count;
         ++index) {
        uint32_t prior;

        if (cm_hir_def_id_equal(call->data.method_call.in_scope_traits[index],
                value_trait->definition)) saw_value = 1;
        if (cm_hir_def_id_equal(call->data.method_call.in_scope_traits[index],
                imported_trait->definition)) saw_imported = 1;
        if (cm_hir_def_id_equal(call->data.method_call.in_scope_traits[index],
                shadowed_trait->definition)) saw_shadowed = 1;
        for (prior = 0u; prior < index; ++prior) {
            assert(!cm_hir_def_id_equal(
                call->data.method_call.in_scope_traits[prior],
                call->data.method_call.in_scope_traits[index]));
        }
    }
    assert(saw_value && saw_imported && !saw_shadowed
        && call->data.method_call.in_scope_trait_count == 2u);
    fixture_destroy(&fixture);

    expect_body_failure(
        "trait Value { fn value<T>(self) -> u32; } "
        "fn main(x: u32) -> u32 { x.value::<u32>() }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
}

static void test_graph_method_call_oom_is_transactional(void)
{
    TestFixture fixture;
    TestHirSnapshot snapshot;

    fixture_init(&fixture,
        "trait Value { fn value(self, other: u32) -> u32; } "
        "fn main(x: u32) -> u32 { x.value(7u32) }");
    hir_snapshot_take(&fixture.hir, &snapshot);
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    /* Trait-scope scratch allocation succeeds; argument storage fails. */
    cm_alloc_fail_after(1u);
    if (setjmp(oom_jump) == 0) {
        (void)lower_fixture_body(&fixture);
        assert(0 && "method argument storage unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    hir_snapshot_assert_unchanged(&fixture.hir, &snapshot);
    hir_snapshot_destroy(&snapshot);
    fixture_destroy(&fixture);
}

static void test_raw_reference_rejections_are_transactional(void)
{
    static const char source[] =
        "fn main(value: u32) -> u32 { &raw const value }";
    TestFixture fixture;
    const CmAst *ast;
    const CmHirBody *body;
    CmAstExpr *block;
    CmAstExpr *raw_reference;
    CmAstExprId raw_reference_id;
    CmHirBodyLowerResult result;

    expect_body_failure(source, CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(mut value: u32) -> u32 { &raw mut value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);

    fixture_init(&fixture, source);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && cm_module_graph_borrow_ast(&fixture.graph,
        fixture.graph_result.root, &ast));
    block = (CmAstExpr *)cm_ast_get_expr(ast,
        (CmAstExprId)body->source_expression_id);
    raw_reference_id = block == NULL || block->kind != CM_AST_EXPR_BLOCK
        ? CM_AST_EXPR_NONE : block->data.block.tail;
    raw_reference = (CmAstExpr *)cm_ast_get_expr(ast, raw_reference_id);
    assert(raw_reference != NULL
        && raw_reference->kind == CM_AST_EXPR_RAW_REFERENCE);
    raw_reference->data.raw_reference.kind = (CmAstRawReferenceKind)99;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_BODY
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    fixture_destroy(&fixture);

    fixture_init(&fixture, source);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && cm_module_graph_borrow_ast(&fixture.graph,
        fixture.graph_result.root, &ast));
    block = (CmAstExpr *)cm_ast_get_expr(ast,
        (CmAstExprId)body->source_expression_id);
    raw_reference_id = block == NULL || block->kind != CM_AST_EXPR_BLOCK
        ? CM_AST_EXPR_NONE : block->data.block.tail;
    raw_reference = (CmAstExpr *)cm_ast_get_expr(ast, raw_reference_id);
    assert(raw_reference != NULL
        && raw_reference->kind == CM_AST_EXPR_RAW_REFERENCE);
    raw_reference->data.raw_reference.operand = raw_reference_id;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_BODY
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    fixture_destroy(&fixture);

    fixture_init(&fixture, source);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && cm_module_graph_borrow_ast(&fixture.graph,
        fixture.graph_result.root, &ast));
    block = (CmAstExpr *)cm_ast_get_expr(ast,
        (CmAstExprId)body->source_expression_id);
    raw_reference_id = block == NULL || block->kind != CM_AST_EXPR_BLOCK
        ? CM_AST_EXPR_NONE : block->data.block.tail;
    raw_reference = (CmAstExpr *)cm_ast_get_expr(ast, raw_reference_id);
    assert(raw_reference != NULL
        && raw_reference->kind == CM_AST_EXPR_RAW_REFERENCE);
    raw_reference->span.end -= 1u;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_BODY
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    fixture_destroy(&fixture);
}

static void test_aggregate_resolver_snapshot_is_transactional(void)
{
    static const char source[] =
        "struct Pair { value: u32 } "
        "fn main(value: u32) -> Pair { Pair { value } }";
    TestFixture fixture;
    TestFixture other;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirLocal *locals;
    size_t expression_count;
    size_t arena_bytes;
    size_t string_count;
    uint32_t local_count;

    fixture_init(&fixture, source);
    fixture_init(&other, source);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL);
    locals = body->locals;
    local_count = body->local_count;
    expression_count = fixture.hir.expressions.len;
    arena_bytes = cm_arena_bytes_used(&fixture.hir.storage);
    string_count = cm_interner_length(&fixture.hir.strings);
    result = cm_hir_lower_body(&fixture.hir, fixture.body, &fixture.graph,
        fixture.graph_result.revision, &other.imports, &fixture.map);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_SOURCE_MISMATCH
        && result.root_expression == CM_HIR_EXPR_NONE
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && body->locals == locals && body->local_count == local_count
        && fixture.hir.expressions.len == expression_count
        && cm_arena_bytes_used(&fixture.hir.storage) == arena_bytes
        && cm_interner_length(&fixture.hir.strings) == string_count
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    fixture_destroy(&other);
    fixture_destroy(&fixture);
}

static void test_named_field_projection_rejections_are_transactional(void)
{
    TestFixture fixture;
    TestFixture other;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirLocal *locals;
    const CmAst *ast;
    CmAstExpr *block;
    CmAstExpr *field;
    CmAstExprId field_id;
    size_t expression_count;
    size_t arena_bytes;
    size_t string_count;
    uint32_t local_count;

    expect_body_failure(
        "struct Pair { value: u32 } "
        "fn main(value: u32) -> u32 { value.missing }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE);
    expect_body_failure(
        "struct Pair { value: u32 } "
        "fn main(pair: Pair) -> u32 { pair.missing }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
    expect_body_failure(
        "struct Pair { value: u32 } "
        "fn main(pair: Pair) -> i32 { pair.value }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_body_failure(
        "mod defs { pub struct Pair { hidden: u32 } } "
        "fn main(pair: defs::Pair) -> u32 { pair.hidden }",
        CM_HIR_BODY_LOWER_INVALID_BODY);

    fixture_init(&fixture,
        "struct Pair { value: u32 } "
        "fn main(pair: Pair) -> u32 { pair.value }");
    fixture_init(&other,
        "struct Pair { value: u32 } "
        "fn main(pair: Pair) -> u32 { pair.value }");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL);
    locals = body->locals;
    local_count = body->local_count;
    expression_count = fixture.hir.expressions.len;
    arena_bytes = cm_arena_bytes_used(&fixture.hir.storage);
    string_count = cm_interner_length(&fixture.hir.strings);
    result = cm_hir_lower_body(&fixture.hir, fixture.body, &fixture.graph,
        fixture.graph_result.revision, &other.imports, &fixture.map);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_SOURCE_MISMATCH
        && result.root_expression == CM_HIR_EXPR_NONE
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && body->locals == locals && body->local_count == local_count
        && fixture.hir.expressions.len == expression_count
        && cm_arena_bytes_used(&fixture.hir.storage) == arena_bytes
        && cm_interner_length(&fixture.hir.strings) == string_count
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    fixture_destroy(&other);
    fixture_destroy(&fixture);

    fixture_init(&fixture,
        "struct Pair { value: u32 } "
        "fn main(pair: Pair) -> u32 { pair.value }");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && cm_module_graph_borrow_ast(&fixture.graph,
        fixture.graph_result.root, &ast));
    block = (CmAstExpr *)cm_ast_get_expr(ast,
        (CmAstExprId)body->source_expression_id);
    field_id = block == NULL || block->kind != CM_AST_EXPR_BLOCK
        ? CM_AST_EXPR_NONE : block->data.block.tail;
    field = (CmAstExpr *)cm_ast_get_expr(ast, field_id);
    assert(field != NULL && field->kind == CM_AST_EXPR_FIELD);
    field->data.field.base = field_id;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_BODY
        && result.root_expression == CM_HIR_EXPR_NONE
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    fixture_destroy(&fixture);
}

static void test_aggregate_payload_oom_is_transactional(void)
{
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirLocal *locals;
    size_t expression_count;
    size_t arena_bytes;
    size_t string_count;
    uint32_t local_count;

    fixture_init(&fixture,
        "struct Pair { value: u32 } "
        "fn main(value: u32) -> Pair { Pair { value } }");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && fixture.hir.expressions.cap == 0u);
    locals = body->locals;
    local_count = body->local_count;
    expression_count = fixture.hir.expressions.len;
    arena_bytes = cm_arena_bytes_used(&fixture.hir.storage);
    string_count = cm_interner_length(&fixture.hir.strings);
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(2u);
    if (setjmp(oom_jump) == 0) {
        (void)lower_fixture_body(&fixture);
        assert(0 && "aggregate transaction allocation survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && body->locals == locals && body->local_count == local_count
        && fixture.hir.expressions.len == expression_count
        && cm_arena_bytes_used(&fixture.hir.storage) == arena_bytes
        && cm_interner_length(&fixture.hir.strings) == string_count
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    fixture_destroy(&fixture);
}

static void test_oom_precedes_body_mutation(void)
{
    TestFixture fixture;
    const CmHirBody *body;
    size_t expression_count;

    fixture_init(&fixture, "fn main() -> i32 { 7i32 }");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && fixture.hir.expressions.cap == 0u);
    expression_count = fixture.hir.expressions.len;
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(oom_jump) == 0) {
        (void)lower_fixture_body(&fixture);
        assert(0 && "body expression reservation unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == expression_count
        && fixture.hir.storage.active_marks.len == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_function_body(void)
{
    static const char source[] =
        "#![feature(no_core)]\n"
        "#![no_core]\n"
        "#![no_main]\n"
        "#[no_mangle]\n"
        "pub extern \"C\" fn main() -> i32 { 7i32 }\n";
    CmSourceSet sources;
    CmSourceId source_id;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirItem *function;
    const CmHirBody *body;
    CmHirBodyLowerResult body_result;

    cm_source_set_init(&sources);
    assert(cm_source_add_memory(&sources, "no-core/lib.rs",
        (const unsigned char *)source, strlen(source), &source_id)
        == CM_SOURCE_OK);
    cm_cfg_set_init(&cfg);
    cm_module_graph_init(&graph);
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = CM_EDITION_2021;
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, source_id,
        &graph_options);
    assert(graph_result.error_count == 0u
        && graph_result.revision != CM_MODULE_GRAPH_REVISION_NONE);
    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.error_count == 0u);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "no_core";
    lower_options.edition = CM_HIR_EDITION_2021;
    lower_result = cm_hir_lower_module_graph(&hir, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u
        && lower_result.lowered_item_count == 1u);
    function = find_function(&hir, "main");
    assert(function != NULL && function->attribute_count == 1u);
    body = cm_hir_get_body(&hir, function->data.function_item.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->source == source_id);
    body_result = cm_hir_lower_body(&hir,
        function->data.function_item.body, &graph, graph_result.revision,
        &imports, &map);
    assert(body_result.status == CM_HIR_BODY_LOWER_OK
        && cm_hir_get_body(&hir, function->data.function_item.body)->state
            == CM_HIR_BODY_TYPED);

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_graph_identity_call_bodies(void)
{
    static const char source[] =
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "#[no_mangle]\n"
        "pub extern \"C\" fn probe(x: u32) -> u32 {\n"
        "    identity::<u32>(x)\n"
        "}\n";
    TestFixture fixture;
    const CmHirItem *identity;
    const CmHirItem *probe;
    const CmHirBody *identity_body;
    const CmHirBody *probe_body;
    const CmHirExpr *root;
    const CmHirExpr *tail;
    const CmHirExpr *argument;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    identity = find_function(&fixture.hir, "identity");
    probe = find_function(&fixture.hir, "probe");
    assert(identity != NULL && probe != NULL
        && identity->generic_parameter_count == 1u
        && identity->data.function_item.signature.parameter_count == 1u
        && probe->data.function_item.signature.parameter_count == 1u);

    result = cm_hir_lower_body(&fixture.hir,
        identity->data.function_item.body, &fixture.graph,
        fixture.graph_result.revision, &fixture.imports, &fixture.map);
    if (result.status != CM_HIR_BODY_LOWER_OK) {
        fprintf(stderr, "identity body lowering failed: %s\n",
            cm_hir_body_lower_status_name(result.status));
    }
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    identity_body = cm_hir_get_body(&fixture.hir,
        identity->data.function_item.body);
    root = identity_body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        identity_body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    assert(identity_body != NULL && identity_body->state == CM_HIR_BODY_TYPED
        && identity_body->local_count == 1u
        && tail != NULL && tail->kind == CM_HIR_EXPR_LOCAL
        && tail->owner_body == identity->data.function_item.body
        && tail->data.local.local_index == 0u
        && tail->type == identity_body->expected_type);

    result = cm_hir_lower_body(&fixture.hir,
        probe->data.function_item.body, &fixture.graph,
        fixture.graph_result.revision, &fixture.imports, &fixture.map);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    probe_body = cm_hir_get_body(&fixture.hir,
        probe->data.function_item.body);
    root = probe_body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        probe_body->root_expression);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    argument = tail == NULL || tail->kind != CM_HIR_EXPR_CALL
        || tail->data.call.argument_count != 1u ? NULL
        : cm_hir_get_expr(&fixture.hir, tail->data.call.arguments[0]);
    assert(probe_body != NULL && probe_body->state == CM_HIR_BODY_TYPED
        && tail != NULL && tail->kind == CM_HIR_EXPR_CALL
        && tail->owner_body == probe->data.function_item.body
        && cm_hir_def_id_equal(tail->data.call.callee,
            identity->definition)
        && tail->data.call.type_substitution_count == 1u
        && tail->data.call.type_substitutions[0]
            == probe_body->expected_type
        && argument != NULL && argument->kind == CM_HIR_EXPR_LOCAL
        && argument->owner_body == probe->data.function_item.body
        && argument->data.local.local_index == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_generic_identity_call_body(void)
{
    static const char source[] =
        "fn identity<T>(x: T) -> T { x }\n"
        "fn caller<T>(x: T) -> T { identity::<T>(x) }\n";
    TestFixture fixture;
    const CmHirItem *identity;
    const CmHirItem *caller;
    const CmHirBody *caller_body;
    const CmHirExpr *root;
    const CmHirExpr *call;
    const CmHirExpr *argument;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "caller");
    identity = find_function(&fixture.hir, "identity");
    caller = find_function(&fixture.hir, "caller");
    assert(identity != NULL && caller != NULL
        && identity->generic_parameter_count == 1u
        && caller->generic_parameter_count == 1u);
    result = cm_hir_lower_body(&fixture.hir,
        identity->data.function_item.body, &fixture.graph,
        fixture.graph_result.revision, &fixture.imports, &fixture.map);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    result = cm_hir_lower_body(&fixture.hir,
        caller->data.function_item.body, &fixture.graph,
        fixture.graph_result.revision, &fixture.imports, &fixture.map);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    caller_body = cm_hir_get_body(&fixture.hir,
        caller->data.function_item.body);
    root = caller_body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        caller_body->root_expression);
    call = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    argument = call == NULL || call->kind != CM_HIR_EXPR_CALL
            || call->data.call.argument_count != 1u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            call->data.call.arguments[0]);
    assert(caller_body != NULL && caller_body->state == CM_HIR_BODY_TYPED
        && call != NULL && call->kind == CM_HIR_EXPR_CALL
        && cm_hir_def_id_equal(call->data.call.callee,
            identity->definition)
        && call->type == caller_body->expected_type
        && call->data.call.type_substitution_count == 1u
        && call->data.call.type_substitutions[0]
            == caller_body->expected_type
        && argument != NULL && argument->kind == CM_HIR_EXPR_LOCAL
        && argument->type == caller_body->expected_type);
    fixture_destroy(&fixture);
}

static void test_graph_generic_identity_call_rejects_wrong_parameter(void)
{
    static const char source[] =
        "fn identity<T>(x: T) -> T { x }\n"
        "fn caller<T, U>(x: T) -> T { identity::<U>(x) }\n";
    TestFixture fixture;
    const CmHirItem *caller;
    CmHirBodyLowerResult result;
    size_t expressions_before;

    fixture_init_named(&fixture, source, "caller");
    caller = find_function(&fixture.hir, "caller");
    assert(caller != NULL && caller->generic_parameter_count == 2u);
    expressions_before = fixture.hir.expressions.len;
    result = cm_hir_lower_body(&fixture.hir,
        caller->data.function_item.body, &fixture.graph,
        fixture.graph_result.revision, &fixture.imports, &fixture.map);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_SUBSTITUTION
        && fixture.hir.expressions.len == expressions_before
        && cm_hir_get_body(&fixture.hir,
            caller->data.function_item.body)->state
            == CM_HIR_BODY_UNLOWERED);
    fixture_destroy(&fixture);
}

static void test_graph_identity_call_recursive_argument(void)
{
    static const char source[] =
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(left: u32, right: u32) -> u32 {\n"
        "    identity::<u32>(left + (1u32 + right))\n"
        "}\n";
    TestFixture fixture;
    const CmHirItem *identity;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *call;
    const CmHirExpr *outer;
    const CmHirExpr *left;
    const CmHirExpr *inner;
    const CmHirExpr *literal;
    const CmHirExpr *right;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    identity = find_function(&fixture.hir, "identity");
    assert(identity != NULL);
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    call = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    outer = call == NULL || call->kind != CM_HIR_EXPR_CALL
        || call->data.call.argument_count != 1u ? NULL
        : cm_hir_get_expr(&fixture.hir, call->data.call.arguments[0]);
    left = outer == NULL || outer->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, outer->data.binary.left);
    inner = outer == NULL || outer->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, outer->data.binary.right);
    literal = inner == NULL || inner->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, inner->data.binary.left);
    right = inner == NULL || inner->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, inner->data.binary.right);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->local_count == 2u && fixture.hir.expressions.len == 7u
        && call != NULL && call->kind == CM_HIR_EXPR_CALL
        && call->owner_body == fixture.body
        && cm_hir_def_id_equal(call->data.call.callee,
            identity->definition)
        && call->data.call.type_substitution_count == 1u
        && call->data.call.type_substitutions[0] == body->expected_type
        && outer != NULL && outer->kind == CM_HIR_EXPR_BINARY
        && outer->owner_body == fixture.body
        && outer->data.binary.operator_kind == CM_HIR_BINARY_ADD
        && left != NULL && left->kind == CM_HIR_EXPR_LOCAL
        && left->owner_body == fixture.body
        && left->data.local.local_index == 0u
        && inner != NULL && inner->kind == CM_HIR_EXPR_BINARY
        && inner->owner_body == fixture.body
        && inner->data.binary.operator_kind == CM_HIR_BINARY_ADD
        && literal != NULL && literal->kind == CM_HIR_EXPR_INTEGER
        && literal->owner_body == fixture.body
        && literal->data.integer.low_bits == 1u
        && literal->data.integer.high_bits == 0u
        && right != NULL && right->kind == CM_HIR_EXPR_LOCAL
        && right->owner_body == fixture.body
        && right->data.local.local_index == 1u);
    fixture_destroy(&fixture);
}

static void test_graph_monomorphic_call_computed_arguments(void)
{
    static const char source[] =
        "fn add_pair(left: u32, right: u32) -> u32 { left + right }\n"
        "fn probe(left: u32, right: u32) -> u32 {\n"
        "    add_pair(left + 1u32, right + 2u32)\n"
        "}\n";
    TestFixture fixture;
    const CmHirItem *add_pair;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *call;
    const CmHirExpr *first;
    const CmHirExpr *second;
    const CmHirExpr *first_local;
    const CmHirExpr *first_constant;
    const CmHirExpr *second_local;
    const CmHirExpr *second_constant;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    add_pair = find_function(&fixture.hir, "add_pair");
    assert(add_pair != NULL);
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    call = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    first = call == NULL || call->kind != CM_HIR_EXPR_CALL
        || call->data.call.argument_count != 2u ? NULL
        : cm_hir_get_expr(&fixture.hir, call->data.call.arguments[0]);
    second = call == NULL || call->kind != CM_HIR_EXPR_CALL
        || call->data.call.argument_count != 2u ? NULL
        : cm_hir_get_expr(&fixture.hir, call->data.call.arguments[1]);
    first_local = first == NULL || first->kind != CM_HIR_EXPR_BINARY
        ? NULL : cm_hir_get_expr(&fixture.hir, first->data.binary.left);
    first_constant = first == NULL || first->kind != CM_HIR_EXPR_BINARY
        ? NULL : cm_hir_get_expr(&fixture.hir, first->data.binary.right);
    second_local = second == NULL || second->kind != CM_HIR_EXPR_BINARY
        ? NULL : cm_hir_get_expr(&fixture.hir, second->data.binary.left);
    second_constant = second == NULL || second->kind != CM_HIR_EXPR_BINARY
        ? NULL : cm_hir_get_expr(&fixture.hir, second->data.binary.right);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.expressions.len == 8u
        && call != NULL && call->kind == CM_HIR_EXPR_CALL
        && call->owner_body == fixture.body
        && cm_hir_def_id_equal(call->data.call.callee,
            add_pair->definition)
        && call->data.call.type_substitution_count == 0u
        && call->data.call.type_substitutions != NULL
        && (const uint32_t *)call->data.call.type_substitutions
            == (const uint32_t *)call->data.call.arguments
        && call->data.call.argument_count == 2u
        && first != NULL && first->kind == CM_HIR_EXPR_BINARY
        && first->owner_body == fixture.body
        && first_local != NULL && first_local->kind == CM_HIR_EXPR_LOCAL
        && first_local->data.local.local_index == 0u
        && first_constant != NULL
        && first_constant->kind == CM_HIR_EXPR_INTEGER
        && first_constant->data.integer.low_bits == 1u
        && second != NULL && second->kind == CM_HIR_EXPR_BINARY
        && second->owner_body == fixture.body
        && second_local != NULL && second_local->kind == CM_HIR_EXPR_LOCAL
        && second_local->data.local.local_index == 1u
        && second_constant != NULL
        && second_constant->kind == CM_HIR_EXPR_INTEGER
        && second_constant->data.integer.low_bits == 2u);
    fixture_destroy(&fixture);
}

static void test_graph_contextual_unsuffixed_call_argument(void)
{
    static const char source[] =
        "fn accept(value: u32) -> u32 { value }\n"
        "fn probe() -> u32 { accept(9) }\n";
    TestFixture fixture;
    const CmHirItem *accept;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *call;
    const CmHirExpr *argument;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    accept = find_function(&fixture.hir, "accept");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    call = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    argument = call == NULL || call->kind != CM_HIR_EXPR_CALL
            || call->data.call.argument_count != 1u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            call->data.call.arguments[0]);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && accept != NULL
        && call != NULL && call->kind == CM_HIR_EXPR_CALL
        && cm_hir_def_id_equal(call->data.call.callee,
            accept->definition)
        && argument != NULL && argument->kind == CM_HIR_EXPR_INTEGER
        && argument->type
            == accept->data.function_item.signature.parameters[0].type
        && argument->data.integer.low_bits == 9u
        && argument->data.integer.high_bits == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_monomorphic_aggregate_call_arguments(void)
{
    static const char source[] =
        "struct Inner { first: u32, second: u32 }\n"
        "struct Outer { inner: Inner, tail: u32 }\n"
        "fn select(value: Outer, bias: u32) -> u32 {\n"
        "    value.inner.second + bias\n"
        "}\n"
        "fn probe(seed: u32) -> u32 {\n"
        "    select(Outer { inner: Inner { first: 1u32, second: seed }, "
            "tail: 2u32 }, 3u32)\n"
        "}\n";
    TestFixture fixture;
    const CmHirItem *select;
    const CmHirItem *outer_item;
    const CmHirItem *inner_item;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *call;
    const CmHirExpr *outer;
    const CmHirExpr *inner;
    const CmHirExpr *seed;
    const CmHirExpr *bias;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    select = find_function(&fixture.hir, "select");
    outer_item = find_struct(&fixture.hir, "Outer");
    inner_item = find_struct(&fixture.hir, "Inner");
    assert(select != NULL && outer_item != NULL && inner_item != NULL
        && select->generic_parameter_count == 0u
        && select->data.function_item.signature.parameter_count == 2u);
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    call = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    outer = call == NULL || call->kind != CM_HIR_EXPR_CALL
            || call->data.call.argument_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            call->data.call.arguments[0]);
    bias = call == NULL || call->kind != CM_HIR_EXPR_CALL
            || call->data.call.argument_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            call->data.call.arguments[1]);
    inner = outer == NULL || outer->kind != CM_HIR_EXPR_AGGREGATE
            || outer->data.aggregate.field_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            outer->data.aggregate.fields[0].value);
    seed = inner == NULL || inner->kind != CM_HIR_EXPR_AGGREGATE
            || inner->data.aggregate.field_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            inner->data.aggregate.fields[1].value);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.expressions.len == 8u
        && call != NULL && call->kind == CM_HIR_EXPR_CALL
        && call->owner_body == fixture.body
        && cm_hir_def_id_equal(call->data.call.callee,
            select->definition)
        && call->data.call.type_substitution_count == 0u
        && call->data.call.argument_count == 2u
        && call->data.call.owned_storage == NULL
        && outer != NULL && outer->kind == CM_HIR_EXPR_AGGREGATE
        && outer->type
            == select->data.function_item.signature.parameters[0].type
        && cm_hir_def_id_equal(outer->data.aggregate.definition,
            outer_item->definition)
        && outer->data.aggregate.owned_storage
            == outer->data.aggregate.fields
        && inner != NULL && inner->kind == CM_HIR_EXPR_AGGREGATE
        && cm_hir_def_id_equal(inner->data.aggregate.definition,
            inner_item->definition)
        && inner->data.aggregate.owned_storage == NULL
        && seed != NULL && seed->kind == CM_HIR_EXPR_LOCAL
        && seed->data.local.local_index == 0u
        && bias != NULL && bias->kind == CM_HIR_EXPR_INTEGER
        && bias->data.integer.low_bits == 3u);
    fixture_destroy(&fixture);

    fixture_init_named(&fixture,
        "struct Outer { value: u32 }\n"
        "fn select(value: Outer) -> u32 { value.value }\n"
        "fn probe(value: Outer) -> u32 { select(value) }\n", "probe");
    select = find_function(&fixture.hir, "select");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    call = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    outer = call == NULL || call->kind != CM_HIR_EXPR_CALL
            || call->data.call.argument_count != 1u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            call->data.call.arguments[0]);
    assert(result.status == CM_HIR_BODY_LOWER_OK && select != NULL
        && call != NULL && call->kind == CM_HIR_EXPR_CALL
        && cm_hir_def_id_equal(call->data.call.callee,
            select->definition)
        && outer != NULL && outer->kind == CM_HIR_EXPR_LOCAL
        && outer->data.local.local_index == 0u
        && outer->type
            == select->data.function_item.signature.parameters[0].type);
    fixture_destroy(&fixture);
}

static void test_graph_nested_monomorphic_call_expression(void)
{
    static const char source[] =
        "fn add_pair(left: u32, right: u32) -> u32 { left + right }\n"
        "fn probe(left: u32, right: u32) -> u32 {\n"
        "    add_pair(add_pair(left + 1u32, right + 2u32), left + 3u32)\n"
        "}\n";
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *outer;
    const CmHirExpr *inner;
    const CmHirExpr *outer_second;
    const uint32_t *storage;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    outer = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    inner = outer == NULL || outer->kind != CM_HIR_EXPR_CALL
        || outer->data.call.argument_count != 2u ? NULL
        : cm_hir_get_expr(&fixture.hir, outer->data.call.arguments[0]);
    outer_second = outer == NULL || outer->kind != CM_HIR_EXPR_CALL
        || outer->data.call.argument_count != 2u ? NULL
        : cm_hir_get_expr(&fixture.hir, outer->data.call.arguments[1]);
    storage = inner == NULL ? NULL : inner->data.call.owned_storage;
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.expressions.len == 12u
        && outer != NULL && outer->kind == CM_HIR_EXPR_CALL
        && outer->owner_body == fixture.body
        && outer->data.call.type_substitution_count == 0u
        && outer->data.call.argument_count == 2u
        && outer->data.call.owned_storage == NULL
        && inner != NULL && inner->kind == CM_HIR_EXPR_CALL
        && inner->owner_body == fixture.body
        && inner->data.call.type_substitution_count == 0u
        && inner->data.call.argument_count == 2u
        && storage != NULL
        && (const uint32_t *)inner->data.call.type_substitutions == storage
        && (const uint32_t *)outer->data.call.type_substitutions
            == storage + 2u
        && outer_second != NULL
        && outer_second->kind == CM_HIR_EXPR_BINARY
        && outer_second->owner_body == fixture.body);
    fixture_destroy(&fixture);
}

static void test_graph_call_nested_under_add_expression(void)
{
    static const char source[] =
        "fn add_pair(left: u32, right: u32) -> u32 { left + right }\n"
        "fn probe(left: u32, right: u32) -> u32 {\n"
        "    add_pair(left + 1u32, right + 2u32) + (left + 3u32)\n"
        "}\n";
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *addition;
    const CmHirExpr *call;
    const CmHirExpr *right;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    addition = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    call = addition == NULL || addition->kind != CM_HIR_EXPR_BINARY
        ? NULL : cm_hir_get_expr(&fixture.hir,
            addition->data.binary.left);
    right = addition == NULL || addition->kind != CM_HIR_EXPR_BINARY
        ? NULL : cm_hir_get_expr(&fixture.hir,
            addition->data.binary.right);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.expressions.len == 12u
        && addition != NULL && addition->kind == CM_HIR_EXPR_BINARY
        && addition->owner_body == fixture.body
        && call != NULL && call->kind == CM_HIR_EXPR_CALL
        && call->owner_body == fixture.body
        && call->data.call.argument_count == 2u
        && call->data.call.owned_storage != NULL
        && right != NULL && right->kind == CM_HIR_EXPR_BINARY
        && right->owner_body == fixture.body);
    fixture_destroy(&fixture);
}

static void test_graph_monomorphic_one_argument_call(void)
{
    static const char source[] =
        "fn bump(value: u32) -> u32 { value + 1u32 }\n"
        "fn probe(value: u32) -> u32 { bump(value + 2u32) }\n";
    TestFixture fixture;
    const CmHirItem *bump;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *call;
    const CmHirExpr *argument;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    bump = find_function(&fixture.hir, "bump");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    call = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    argument = call == NULL || call->kind != CM_HIR_EXPR_CALL
        || call->data.call.argument_count != 1u ? NULL
        : cm_hir_get_expr(&fixture.hir, call->data.call.arguments[0]);
    assert(result.status == CM_HIR_BODY_LOWER_OK && bump != NULL
        && call != NULL && call->kind == CM_HIR_EXPR_CALL
        && cm_hir_def_id_equal(call->data.call.callee, bump->definition)
        && call->data.call.type_substitution_count == 0u
        && call->data.call.argument_count == 1u
        && argument != NULL && argument->kind == CM_HIR_EXPR_BINARY
        && argument->owner_body == fixture.body);
    fixture_destroy(&fixture);
}

static void expect_probe_body_failure(const char *source,
    CmHirBodyLowerStatus expected)
{
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;

    fixture_init_named(&fixture, source, "probe");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == expected && body != NULL
        && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_identity_call_rejections(void)
{
    expect_probe_body_failure(
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(x: u32) -> u32 { missing::<u32>(x) }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
    expect_probe_body_failure(
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(x: u32) -> u32 { identity(x) }",
        CM_HIR_BODY_LOWER_INVALID_SUBSTITUTION);
    expect_probe_body_failure(
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(x: u32) -> u32 { identity::<u64>(x) }",
        CM_HIR_BODY_LOWER_INVALID_SUBSTITUTION);
    expect_probe_body_failure(
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(x: u32, y: u32) -> u32 { identity::<u32>(x * y) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_probe_body_failure(
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(x: u32) -> u32 { identity::<u32>(x + 1i32) }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_probe_body_failure(
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(x: u32) -> u32 { identity::<u32>(x + missing) }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
    expect_probe_body_failure(
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(x: u64) -> u32 { identity::<u32>(x) }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
}

static void test_graph_monomorphic_call_rejections(void)
{
    expect_probe_body_failure(
        "fn add_pair(x: u32, y: u32) -> u32 { x + y }\n"
        "fn probe(x: u32, y: u32) -> u32 { add_pair::<u32>(x, y) }",
        CM_HIR_BODY_LOWER_INVALID_SUBSTITUTION);
    expect_probe_body_failure(
        "fn add_pair(x: u32, y: u32) -> u32 { x + y }\n"
        "fn probe(x: u32) -> u32 { add_pair(x) }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_probe_body_failure(
        "fn add_pair(x: u32, y: u32) -> u32 { x + y }\n"
        "fn probe(x: u32, y: u32) -> u32 { add_pair(x, y, 1u32) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_probe_body_failure(
        "fn add_pair(x: u32, y: u32) -> u32 { x + y }\n"
        "fn probe(x: u32, y: u32) -> u32 { add_pair(x * 1u32, y) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_probe_body_failure(
        "fn add_pair(x: u32, y: u32) -> u32 { x + y }\n"
        "fn probe(x: u32, y: u32) -> u32 {\n"
        "    add_pair(add_pair(x + 1u32, y * 2u32), x + 3u32)\n"
        "}", CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_probe_body_failure(
        "fn add_pair(x: u32, y: u32) -> u32 { x + y }\n"
        "fn probe(x: u64, y: u32) -> u32 { add_pair(x, y) }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_probe_body_failure(
        "fn pair<T>(x: T, y: T) -> T { x }\n"
        "fn probe(x: u32, y: u32) -> u32 { pair::<u32>(x, y) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_probe_body_failure(
        "struct Pair { value: u32 }\n"
        "extern \"C\" fn select(value: Pair) -> u32 { value.value }\n"
        "fn probe(value: Pair) -> u32 { select(value) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_probe_body_failure(
        "struct Generic<T> { value: T }\n"
        "fn select(value: Generic<u32>) -> u32 { value.value }\n"
        "fn probe(value: Generic<u32>) -> u32 { select(value) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE);
    expect_probe_body_failure(
        "struct Pair { value: u32 }\n"
        "struct Other { value: u32 }\n"
        "fn select(value: Pair) -> u32 { value.value }\n"
        "fn probe(value: u32) -> u32 { select(Other { value }) }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_probe_body_failure(
        "struct Pair { first: u32, second: u32 }\n"
        "fn select(value: Pair) -> u32 { value.first }\n"
        "fn probe(value: u32) -> u32 { select(Pair { first: value }) }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_probe_body_failure(
        "struct Pair { value: u32 }\n"
        "fn make(value: u32) -> Pair { Pair { value } }\n"
        "fn probe(value: u32) -> Pair { make(value) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
}

static void test_graph_nested_call_cycle_is_transactional(void)
{
    static const char source[] =
        "fn add_pair(x: u32, y: u32) -> u32 { x + y }\n"
        "fn probe(x: u32, y: u32) -> u32 {\n"
        "    add_pair(add_pair(x + 1u32, y + 2u32), x + 3u32)\n"
        "}\n";
    TestFixture fixture;
    const CmAst *ast;
    const CmHirBody *body;
    CmAstExpr *block;
    CmAstExpr *call;
    CmAstExprId call_id;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && cm_module_graph_borrow_ast(&fixture.graph,
        fixture.graph_result.root, &ast));
    block = (CmAstExpr *)cm_ast_get_expr(ast,
        (CmAstExprId)body->source_expression_id);
    call_id = block == NULL || block->kind != CM_AST_EXPR_BLOCK
        ? CM_AST_EXPR_NONE : block->data.block.tail;
    call = (CmAstExpr *)cm_ast_get_expr(ast, call_id);
    assert(call != NULL && call->kind == CM_AST_EXPR_CALL
        && call->data.call.argument_count == 2u);
    call->data.call.arguments[0] = call_id;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_BODY
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_monomorphic_call_oom_is_transactional(void)
{
    static const char source[] =
        "fn add_pair(x: u32, y: u32) -> u32 { x + y }\n"
        "fn probe(x: u32, y: u32) -> u32 {\n"
        "    add_pair(add_pair(x + 1u32, y + 2u32), x + 3u32)\n"
        "}\n";
    TestFixture fixture;
    const CmHirBody *body;

    fixture_init_named(&fixture, source, "probe");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && fixture.hir.expressions.len == 0u
        && fixture.hir.expressions.cap == 0u);
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    /* Expression reservation succeeds; the complete call batch fails. */
    cm_alloc_fail_after(1u);
    if (setjmp(oom_jump) == 0) {
        (void)lower_fixture_body(&fixture);
        assert(0 && "monomorphic call payload unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u
        && fixture.hir.storage.active_marks.len == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_identity_call_argument_graph_is_transactional(void)
{
    static const char source[] =
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(x: u32, y: u32) -> u32 { identity::<u32>(x + y) }\n";
    TestFixture fixture;
    const CmAst *ast;
    const CmHirBody *body;
    CmAstExpr *block;
    CmAstExpr *call;
    CmAstExpr *argument;
    CmAstExprId argument_id;
    CmHirBodyLowerResult result;

    fixture_init_named(&fixture, source, "probe");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && cm_module_graph_borrow_ast(&fixture.graph,
        fixture.graph_result.root, &ast));
    block = (CmAstExpr *)cm_ast_get_expr(ast,
        (CmAstExprId)body->source_expression_id);
    call = block == NULL || block->kind != CM_AST_EXPR_BLOCK
        ? NULL : (CmAstExpr *)cm_ast_get_expr(ast, block->data.block.tail);
    argument_id = call == NULL || call->kind != CM_AST_EXPR_CALL
        || call->data.call.argument_count != 1u ? CM_AST_EXPR_NONE
        : call->data.call.arguments[0];
    argument = (CmAstExpr *)cm_ast_get_expr(ast, argument_id);
    assert(argument != NULL && argument->kind == CM_AST_EXPR_BINARY);
    argument->data.binary.left = argument_id;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_BODY
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u);
    fixture_destroy(&fixture);

    fixture_init_named(&fixture, source, "probe");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && cm_module_graph_borrow_ast(&fixture.graph,
        fixture.graph_result.root, &ast));
    block = (CmAstExpr *)cm_ast_get_expr(ast,
        (CmAstExprId)body->source_expression_id);
    call = block == NULL || block->kind != CM_AST_EXPR_BLOCK
        ? NULL : (CmAstExpr *)cm_ast_get_expr(ast, block->data.block.tail);
    argument_id = call == NULL || call->kind != CM_AST_EXPR_CALL
        || call->data.call.argument_count != 1u ? CM_AST_EXPR_NONE
        : call->data.call.arguments[0];
    argument = (CmAstExpr *)cm_ast_get_expr(ast, argument_id);
    assert(argument != NULL && argument->kind == CM_AST_EXPR_BINARY);
    argument->data.binary.right = CM_AST_EXPR_NONE;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_INVALID_BODY
        && body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_identity_call_oom_is_transactional(void)
{
    static const char source[] =
        "pub const fn identity<T>(x: T) -> T { x }\n"
        "fn probe(x: u32, y: u32) -> u32 {\n"
        "    identity::<u32>(x + (1u32 + y))\n"
        "}\n";
    TestFixture fixture;
    const CmHirBody *body;
    size_t expression_count;

    fixture_init_named(&fixture, source, "probe");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && fixture.hir.expressions.len == 0u
        && fixture.hir.expressions.cap == 0u);
    expression_count = fixture.hir.expressions.len;
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    /* Expression-vector reservation succeeds; call-payload allocation fails. */
    cm_alloc_fail_after(1u);
    if (setjmp(oom_jump) == 0) {
        (void)lower_fixture_body(&fixture);
        assert(0 && "call payload allocation unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == expression_count
        && fixture.hir.storage.active_marks.len == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_u32_add_body(void)
{
    static const char source[] =
        "pub extern \"C\" fn add(x: u32, y: u32) -> u32 { x + y }\n";
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *binary;
    const CmHirExpr *left;
    const CmHirExpr *right;
    FILE *stream;
    char *dump;
    char binary_record[160];

    fixture_init_named(&fixture, source, "add");
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    binary = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    left = binary == NULL || binary->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, binary->data.binary.left);
    right = binary == NULL || binary->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, binary->data.binary.right);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->local_count == 2u
        && fixture.hir.expressions.len == 4u
        && binary != NULL && binary->kind == CM_HIR_EXPR_BINARY
        && binary->owner_body == fixture.body
        && binary->data.binary.operator_kind == CM_HIR_BINARY_ADD
        && left != NULL && left->kind == CM_HIR_EXPR_LOCAL
        && left->owner_body == fixture.body
        && left->data.local.local_index == 0u
        && right != NULL && right->kind == CM_HIR_EXPR_LOCAL
        && right->owner_body == fixture.body
        && right->data.local.local_index == 1u);

    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, &fixture.hir) == 0);
    dump = read_dump(stream);
    assert(strncmp(dump, "hir-v27\n", strlen("hir-v27\n")) == 0);
    assert(snprintf(binary_record, sizeof(binary_record),
        "expr#3 binary type=ty#%u operator=add left=expr#1 "
        "right=expr#2 owner=body#1",
        (unsigned int)body->expected_type) > 0);
    assert(strstr(dump, binary_record) != NULL);
    free(dump);
    assert(fclose(stream) == 0);
    fixture_destroy(&fixture);
}

static void test_graph_nested_u32_add_body(void)
{
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *outer;
    const CmHirExpr *inner;
    const CmHirExpr *literal;

    fixture_init_named(&fixture,
        "fn add(x: u32, y: u32) -> u32 { (x + (1u32 + y)) }", "add");
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    outer = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    inner = outer == NULL || outer->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, outer->data.binary.right);
    literal = inner == NULL || inner->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, inner->data.binary.left);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.expressions.len == 6u
        && outer != NULL && outer->kind == CM_HIR_EXPR_BINARY
        && inner != NULL && inner->kind == CM_HIR_EXPR_BINARY
        && literal != NULL && literal->kind == CM_HIR_EXPR_INTEGER
        && literal->type == body->expected_type
        && literal->data.integer.low_bits == 1u
        && literal->data.integer.high_bits == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_u32_subtract_body(void)
{
    static const char source[] =
        "pub extern \"C\" fn sub(end: u32, start: u32) -> u32 {\n"
        "    let count: u32 = end - start;\n"
        "    count\n"
        "}\n";
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *initializer;
    FILE *stream;
    char *dump;

    fixture_init_named(&fixture, source, "sub");
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    initializer = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
            || root->data.block.statement_count != 1u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.statements[0].data.let_statement.initializer);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->local_count == 3u
        && root != NULL && root->kind == CM_HIR_EXPR_BLOCK
        && initializer != NULL && initializer->kind == CM_HIR_EXPR_BINARY
        && initializer->data.binary.operator_kind
            == CM_HIR_BINARY_SUBTRACT);

    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, &fixture.hir) == 0);
    dump = read_dump(stream);
    assert(strstr(dump, "operator=subtract") != NULL);
    free(dump);
    assert(fclose(stream) == 0);
    fixture_destroy(&fixture);
}

static void test_graph_u32_equal_if_else_body(void)
{
    static const char source[] =
        "fn choose(left: u32, right: u32) -> u32 {\n"
        "    if left == right { left + 1u32 } else { right - 1u32 }\n"
        "}\n";
    TestFixture fixture;
    CmHirBodyLowerResult result;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *if_expression;
    const CmHirExpr *condition;
    const CmHirExpr *then_expression;
    const CmHirExpr *else_expression;
    const CmHirExpr *then_value;
    const CmHirExpr *else_value;
    CmHirTypeId bool_type;
    FILE *stream;
    char *dump;

    fixture_init_named(&fixture, source, "choose");
    assert(bool_type_count(&fixture.hir, NULL) == 0u);
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    if_expression = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    condition = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.condition);
    then_expression = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.then_expression);
    else_expression = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.else_expression);
    then_value = then_expression == NULL
            || then_expression->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            then_expression->data.block.tail_expression);
    else_value = else_expression == NULL
            || else_expression->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            else_expression->data.block.tail_expression);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.expressions.len == 13u
        && bool_type_count(&fixture.hir, &bool_type) == 1u
        && if_expression != NULL && if_expression->kind == CM_HIR_EXPR_IF
        && if_expression->owner_body == fixture.body
        && if_expression->type == body->expected_type
        && source_span_is(source, if_expression->span,
            "if left == right { left + 1u32 } else { right - 1u32 }")
        && condition != NULL && condition->kind == CM_HIR_EXPR_BINARY
        && condition->type == bool_type
        && condition->data.binary.operator_kind == CM_HIR_BINARY_EQUAL
        && source_span_is(source, condition->span, "left == right")
        && then_expression != NULL
        && then_expression->kind == CM_HIR_EXPR_BLOCK
        && then_expression->type == body->expected_type
        && source_span_is(source, then_expression->span,
            "{ left + 1u32 }")
        && else_expression != NULL
        && else_expression->kind == CM_HIR_EXPR_BLOCK
        && else_expression->type == body->expected_type
        && source_span_is(source, else_expression->span,
            "{ right - 1u32 }")
        && then_value != NULL && then_value->kind == CM_HIR_EXPR_BINARY
        && then_value->data.binary.operator_kind == CM_HIR_BINARY_ADD
        && else_value != NULL && else_value->kind == CM_HIR_EXPR_BINARY
        && else_value->data.binary.operator_kind
            == CM_HIR_BINARY_SUBTRACT);

    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, &fixture.hir) == 0);
    dump = read_dump(stream);
    assert(strstr(dump, "operator=equal") != NULL
        && strstr(dump, " if type=") != NULL
        && strstr(dump, "condition=expr#") != NULL
        && strstr(dump, " then=expr#") != NULL
        && strstr(dump, " else=expr#") != NULL);
    free(dump);
    assert(fclose(stream) == 0);
    fixture_destroy(&fixture);
}

static void test_graph_u32_equal_if_reuses_bool_type(void)
{
    static const char source[] =
        "fn marker(_: bool) -> u32 { 0u32 }\n"
        "fn main(left: u32, right: u32) -> u32 {\n"
        "    if left == right { left } else { right }\n"
        "}\n";
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *if_expression;
    const CmHirExpr *condition;
    CmHirBodyLowerResult result;
    CmHirTypeId bool_type;
    size_t type_count;

    fixture_init(&fixture, source);
    assert(bool_type_count(&fixture.hir, &bool_type) == 1u);
    type_count = fixture.hir.types.len;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    if_expression = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    condition = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.condition);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && fixture.hir.types.len == type_count
        && bool_type_count(&fixture.hir, NULL) == 1u
        && condition != NULL && condition->type == bool_type);
    fixture_destroy(&fixture);
}

static void test_graph_u32_equal_if_rejections(void)
{
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { if x { x } else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { if x != y { x } else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { if x < y { x } else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { if x == y { x } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { "
        "if x == y { x } else if y == x { y } else { x } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { "
        "if let value = x { value } else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { "
        "if x == y { let value: u32 = x; value } else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { "
        "if x == y { 1i32 } else { y } }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { "
        "if x == y {} else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn id(value: u32) -> u32 { value } "
        "fn main(x: u32, y: u32) -> u32 { "
        "if id(x) == y { x } else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn id(value: u32) -> u32 { value } "
        "fn main(x: u32, y: u32) -> u32 { "
        "if x == y { id(x) } else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { "
        "if x == y { if y == x { x } else { y } } else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { "
        "let value: u32 = x; if value == y { value } else { y } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { "
        "1u32 + (if x == y { x } else { y }) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn take(value: u32) -> u32 { value } "
        "fn main(x: u32, y: u32) -> u32 { "
        "take(if x == y { x } else { y }) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { "
        "let value: u32 = if x == y { x } else { y }; value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> bool { x == y }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE);
}

static void test_u32_equal_if_model_invariants(void)
{
    TestFixture fixture;
    const CmHirBody *body;
    CmHirType bool_type_value;
    CmHirTypeId bool_type;
    CmHirExpr block;
    CmHirExprId left;
    CmHirExprId right;
    CmHirExprId condition;
    CmHirExprId then_value;
    CmHirExprId then_block;
    CmHirExprId else_value;
    CmHirExprId else_block;
    CmHirExprId if_expression;
    CmHirExprId invalid;
    uint32_t start;

    fixture_init(&fixture,
        "fn main(left: u32, right: u32) -> u32 {                  left }");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->local_count == 2u && bool_type_count(&fixture.hir, NULL)
            == 0u);
    memset(&bool_type_value, 0, sizeof(bool_type_value));
    bool_type_value.kind = CM_HIR_TYPE_BOOL_KIND;
    bool_type_value.span = body->span;
    assert(cm_hir_add_type(&fixture.hir, &bool_type_value, &bool_type)
        == CM_HIR_OK);
    start = body->span.start + 1u;
    assert(cm_hir_body_add_local_expression(&fixture.hir, fixture.body, 0u,
        body->expected_type, (CmSpan){ fixture.source, start, start + 1u },
        &left) == CM_HIR_OK);
    assert(cm_hir_body_add_local_expression(&fixture.hir, fixture.body, 1u,
        body->expected_type,
        (CmSpan){ fixture.source, start + 2u, start + 3u }, &right)
        == CM_HIR_OK);
    invalid = 99u;
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        CM_HIR_BINARY_EQUAL, left, right, body->expected_type,
        (CmSpan){ fixture.source, start, start + 3u }, &invalid)
        == CM_HIR_INVARIANT_VIOLATION && invalid == CM_HIR_EXPR_NONE);
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        CM_HIR_BINARY_EQUAL, left, right, bool_type,
        (CmSpan){ fixture.source, start, start + 3u }, &condition)
        == CM_HIR_OK);
    assert(cm_hir_body_add_local_expression(&fixture.hir, fixture.body, 0u,
        body->expected_type,
        (CmSpan){ fixture.source, start + 5u, start + 6u }, &then_value)
        == CM_HIR_OK);
    memset(&block, 0, sizeof(block));
    block.kind = CM_HIR_EXPR_BLOCK;
    block.owner_body = fixture.body;
    block.type = body->expected_type;
    block.span = (CmSpan){ fixture.source, start + 4u, start + 7u };
    block.data.block.tail_expression = then_value;
    assert(cm_hir_add_expr(&fixture.hir, &block, &then_block) == CM_HIR_OK);
    assert(cm_hir_body_add_local_expression(&fixture.hir, fixture.body, 1u,
        body->expected_type,
        (CmSpan){ fixture.source, start + 9u, start + 10u }, &else_value)
        == CM_HIR_OK);
    block.span = (CmSpan){ fixture.source, start + 8u, start + 11u };
    block.data.block.tail_expression = else_value;
    assert(cm_hir_add_expr(&fixture.hir, &block, &else_block) == CM_HIR_OK);
    assert(cm_hir_body_add_if_expression(&fixture.hir, fixture.body, left,
        then_block, else_block, body->expected_type,
        (CmSpan){ fixture.source, start, start + 11u }, &invalid)
        == CM_HIR_INVARIANT_VIOLATION && invalid == CM_HIR_EXPR_NONE);
    assert(cm_hir_body_add_if_expression(&fixture.hir, fixture.body,
        condition, then_value, else_block, body->expected_type,
        (CmSpan){ fixture.source, start, start + 11u }, &invalid)
        == CM_HIR_INVARIANT_VIOLATION && invalid == CM_HIR_EXPR_NONE);
    assert(cm_hir_body_add_if_expression(&fixture.hir, fixture.body,
        condition, then_block, else_block, bool_type,
        (CmSpan){ fixture.source, start, start + 11u }, &invalid)
        == CM_HIR_INVARIANT_VIOLATION && invalid == CM_HIR_EXPR_NONE);
    assert(cm_hir_body_add_if_expression(&fixture.hir, fixture.body,
        condition, then_block, else_block, body->expected_type,
        (CmSpan){ fixture.source, start, start + 11u }, &if_expression)
        == CM_HIR_OK);
    assert(cm_hir_get_expr(&fixture.hir, if_expression) != NULL
        && cm_hir_get_expr(&fixture.hir, if_expression)->kind
            == CM_HIR_EXPR_IF);
    fixture_destroy(&fixture);
}

static void test_graph_u32_equal_if_oom_is_transactional(void)
{
    TestFixture fixture;
    const CmHirBody *body;
    size_t type_count;

    fixture_init(&fixture,
        "fn main(x: u32, y: u32) -> u32 { "
        "if x == y { x } else { y } }");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && fixture.hir.expressions.len == 0u
        && bool_type_count(&fixture.hir, NULL) == 0u);
    type_count = fixture.hir.types.len;
    /* Force canonical-bool reservation to be the first allocation. */
    fixture.hir.types.cap = fixture.hir.types.len;
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(oom_jump) == 0) {
        (void)lower_fixture_body(&fixture);
        assert(0 && "bool type reservation unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u
        && fixture.hir.types.len == type_count
        && bool_type_count(&fixture.hir, NULL) == 0u
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_usize_scalar_body(void)
{
    static const char source[] =
        "fn add_pair(left: usize, right: usize) -> usize { left + right }\n"
        "fn probe(left: usize, right: usize) -> usize {\n"
        "    let first: usize = left - 0usize;\n"
        "    let combined: usize = add_pair(first, 4294967296usize);\n"
        "    combined + 18446744073709551615usize\n"
        "}\n";
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *first;
    const CmHirExpr *combined;
    const CmHirExpr *wide;
    const CmHirExpr *tail;
    const CmHirExpr *maximum;
    CmHirBodyLowerResult result;
    FILE *stream;
    char *dump;

    fixture_init_named(&fixture, source, "probe");
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    first = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
            || root->data.block.statement_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.statements[0].data.let_statement.initializer);
    combined = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
            || root->data.block.statement_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.statements[1].data.let_statement.initializer);
    wide = combined == NULL || combined->kind != CM_HIR_EXPR_CALL
            || combined->data.call.argument_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            combined->data.call.arguments[1]);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    maximum = tail == NULL || tail->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, tail->data.binary.right);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->local_count == 4u && body->parameter_count == 2u
        && hir_type_is_usize(&fixture.hir, body->locals[0].type)
        && hir_type_is_usize(&fixture.hir, body->locals[1].type)
        && body->locals[2].type == body->expected_type
        && body->locals[3].type == body->expected_type
        && first != NULL && first->kind == CM_HIR_EXPR_BINARY
        && first->data.binary.operator_kind == CM_HIR_BINARY_SUBTRACT
        && combined != NULL && combined->kind == CM_HIR_EXPR_CALL
        && combined->data.call.type_substitution_count == 0u
        && wide != NULL && wide->kind == CM_HIR_EXPR_INTEGER
        && hir_type_is_usize(&fixture.hir, wide->type)
        && wide->data.integer.low_bits == UINT64_C(4294967296)
        && wide->data.integer.high_bits == 0u
        && tail != NULL && tail->kind == CM_HIR_EXPR_BINARY
        && tail->data.binary.operator_kind == CM_HIR_BINARY_ADD
        && maximum != NULL && maximum->kind == CM_HIR_EXPR_INTEGER
        && maximum->type == body->expected_type
        && maximum->data.integer.low_bits == UINT64_MAX
        && maximum->data.integer.high_bits == 0u);

    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, &fixture.hir) == 0);
    dump = read_dump(stream);
    assert(strstr(dump, "type#1 usize") != NULL
        && strstr(dump, "bits=0x0000000000000000:0000000100000000")
            != NULL
        && strstr(dump, "bits=0x0000000000000000:ffffffffffffffff")
            != NULL);
    free(dump);
    assert(fclose(stream) == 0);
    fixture_destroy(&fixture);
}

static void test_graph_usize_less_if_else_body(void)
{
    static const char source[] =
        "fn choose(left: usize, right: usize) -> usize {\n"
        "    if left < right { left + 1usize } else { right - 1usize }\n"
        "}\n";
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *if_expression;
    const CmHirExpr *condition;
    const CmHirExpr *then_expression;
    const CmHirExpr *else_expression;
    CmHirBodyLowerResult result;
    CmHirTypeId bool_type;
    FILE *stream;
    char *dump;

    fixture_init_named(&fixture, source, "choose");
    assert(bool_type_count(&fixture.hir, NULL) == 0u);
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    if_expression = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.tail_expression);
    condition = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.condition);
    then_expression = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.then_expression);
    else_expression = if_expression == NULL
            || if_expression->kind != CM_HIR_EXPR_IF
        ? NULL : cm_hir_get_expr(&fixture.hir,
            if_expression->data.if_expr.else_expression);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && bool_type_count(&fixture.hir, &bool_type) == 1u
        && if_expression != NULL && if_expression->kind == CM_HIR_EXPR_IF
        && if_expression->type == body->expected_type
        && condition != NULL && condition->kind == CM_HIR_EXPR_BINARY
        && condition->type == bool_type
        && condition->data.binary.operator_kind == CM_HIR_BINARY_LESS
        && source_span_is(source, condition->span, "left < right")
        && then_expression != NULL
        && then_expression->kind == CM_HIR_EXPR_BLOCK
        && then_expression->type == body->expected_type
        && else_expression != NULL
        && else_expression->kind == CM_HIR_EXPR_BLOCK
        && else_expression->type == body->expected_type);
    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, &fixture.hir) == 0);
    dump = read_dump(stream);
    assert(strstr(dump, "operator=less") != NULL);
    free(dump);
    assert(fclose(stream) == 0);
    fixture_destroy(&fixture);
}

static void test_graph_usize_rejections(void)
{
    expect_body_failure("fn main() -> usize { 1u32 }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure("fn main() -> usize { 1u64 }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure("fn main() -> usize { 0x10usize }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure("fn main() -> usize { 1usize + 2_usize }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure(
        "fn main() -> usize { 18446744073709551616usize }",
        CM_HIR_BODY_LOWER_LITERAL_OUT_OF_RANGE);
    expect_body_failure(
        "fn main() -> usize { 18446744073709551616 }",
        CM_HIR_BODY_LOWER_LITERAL_OUT_OF_RANGE);
    expect_body_failure(
        "fn main(left: usize, right: usize) -> usize { left * right }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_body_failure(
        "fn main(left: u32) -> usize { left }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_body_failure(
        "fn main(left: usize) -> usize { let value: u32 = 1u32; left }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE);
    expect_body_failure(
        "fn mixed(value: u32) -> usize { 1usize } "
        "fn main(value: u32) -> usize { mixed(value) }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE);
    expect_body_failure(
        "fn identity<T>(value: T) -> T { value } "
        "fn main(value: usize) -> usize { identity::<usize>(value) }",
        CM_HIR_BODY_LOWER_INVALID_SUBSTITUTION);
    expect_body_failure(
        "fn main(left: usize, right: usize) -> usize { "
        "if left == right { left } else { right } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_body_failure(
        "fn main(left: usize, right: usize) -> usize { "
        "if left < right { left } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: usize, right: usize) -> usize { "
        "if left < right { let value: usize = left; value } "
        "else { right } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32, right: u32) -> u32 { "
        "if left < right { left } else { right } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_body_failure(
        "fn main(left: usize, right: usize) -> usize { "
        "if left < 1u32 { left } else { right } }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure(
        "fn id(value: usize) -> usize { value } "
        "fn main(left: usize, right: usize) -> usize { "
        "if id(left) < right { left } else { right } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn id(value: usize) -> usize { value } "
        "fn main(left: usize, right: usize) -> usize { "
        "if left < right { id(left) } else { right } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: usize, right: usize) -> usize { "
        "if left < right { if right < left { left } else { right } } "
        "else { right } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
}

static void test_usize_binary_model_invariants(void)
{
    TestFixture fixture;
    const CmHirBody *body;
    CmHirType bool_value;
    CmHirTypeId bool_type;
    CmHirExprId left;
    CmHirExprId right;
    CmHirExprId other;
    CmHirExprId binary;
    CmHirExprId invalid;
    uint32_t start;

    fixture_init(&fixture,
        "fn main(left: usize, right: usize, other: u32) -> usize { left }");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->local_count == 3u);
    memset(&bool_value, 0, sizeof(bool_value));
    bool_value.kind = CM_HIR_TYPE_BOOL_KIND;
    bool_value.span = body->span;
    assert(cm_hir_add_type(&fixture.hir, &bool_value, &bool_type)
        == CM_HIR_OK);
    start = body->span.start + 1u;
    assert(cm_hir_body_add_local_expression(&fixture.hir, fixture.body, 0u,
        body->locals[0].type,
        (CmSpan){ fixture.source, start, start + 1u }, &left) == CM_HIR_OK);
    assert(cm_hir_body_add_local_expression(&fixture.hir, fixture.body, 1u,
        body->locals[1].type,
        (CmSpan){ fixture.source, start + 2u, start + 3u }, &right)
        == CM_HIR_OK);
    assert(cm_hir_body_add_local_expression(&fixture.hir, fixture.body, 2u,
        body->locals[2].type,
        (CmSpan){ fixture.source, start + 2u, start + 3u }, &other)
        == CM_HIR_OK);
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        CM_HIR_BINARY_ADD, left, right, body->expected_type,
        (CmSpan){ fixture.source, start, start + 3u }, &binary) == CM_HIR_OK);
    invalid = 99u;
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        CM_HIR_BINARY_ADD, left, other, body->expected_type,
        (CmSpan){ fixture.source, start, start + 3u }, &invalid)
        == CM_HIR_INVARIANT_VIOLATION && invalid == CM_HIR_EXPR_NONE);
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        CM_HIR_BINARY_EQUAL, left, right, bool_type,
        (CmSpan){ fixture.source, start, start + 3u }, &invalid)
        == CM_HIR_INVARIANT_VIOLATION && invalid == CM_HIR_EXPR_NONE);
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        CM_HIR_BINARY_LESS, other, other, bool_type,
        (CmSpan){ fixture.source, start + 2u, start + 3u }, &invalid)
        == CM_HIR_INVARIANT_VIOLATION && invalid == CM_HIR_EXPR_NONE);
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        CM_HIR_BINARY_LESS, left, right, bool_type,
        (CmSpan){ fixture.source, start, start + 3u }, &binary) == CM_HIR_OK);
    fixture_destroy(&fixture);
}

static void test_graph_usize_less_if_oom_is_transactional(void)
{
    TestFixture fixture;
    const CmHirBody *body;
    size_t type_count;

    fixture_init(&fixture,
        "fn main(left: usize, right: usize) -> usize { "
        "if left < right { left } else { right } }");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && fixture.hir.expressions.len == 0u
        && bool_type_count(&fixture.hir, NULL) == 0u);
    type_count = fixture.hir.types.len;
    fixture.hir.types.cap = fixture.hir.types.len;
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(oom_jump) == 0) {
        (void)lower_fixture_body(&fixture);
        assert(0 && "usize bool type reservation unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u
        && fixture.hir.types.len == type_count
        && bool_type_count(&fixture.hir, NULL) == 0u
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    fixture_destroy(&fixture);
}

static void test_graph_u32_add_rejections(void)
{
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { x * y }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { x + 1i32 }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { x + 1_0 }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure(
        "fn main(x: u32, y: u32) -> u32 { x + missing }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
    expect_body_failure(
        "fn main(x: u64, y: u64) -> u64 { x + y }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE);
    expect_body_failure(
        "fn main() -> u32 { 4294967296u32 }",
        CM_HIR_BODY_LOWER_LITERAL_OUT_OF_RANGE);
    expect_body_failure(
        "fn main() -> u32 { 4294967296 }",
        CM_HIR_BODY_LOWER_LITERAL_OUT_OF_RANGE);
    expect_body_failure("fn main() -> u32 { 0x10 }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
    expect_body_failure("fn main() -> u32 { 1.0 }",
        CM_HIR_BODY_LOWER_INVALID_LITERAL);
}

static void test_graph_explicit_u32_let_block(void)
{
    static const char source[] =
        "fn add_pair(left: u32, right: u32) -> u32 { left + right }\n"
        "fn probe(left: u32, right: u32) -> u32 {\n"
        "    let first: u32 = left + 1;\n"
        "    let combined: u32 = add_pair(first, right + 2);\n"
        "    combined + (left + 3)\n"
        "}\n";
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *first;
    const CmHirExpr *first_left;
    const CmHirExpr *combined;
    const CmHirExpr *combined_first;
    const CmHirExpr *combined_right;
    const CmHirExpr *tail;
    const CmHirExpr *tail_combined;
    const CmHirExpr *tail_right;
    const CmHirExpr *tail_left;
    CmHirBodyLowerResult result;
    FILE *stream;
    char *dump;

    fixture_init_named(&fixture, source, "probe");
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->local_count == 2u && fixture.hir.expressions.len == 0u);
    result = lower_fixture_body(&fixture);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && result.root_expression != CM_HIR_EXPR_NONE);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    assert(body != NULL && body->state == CM_HIR_BODY_TYPED
        && body->local_count == 4u && body->parameter_count == 2u
        && body->locals[0].parameter_index == 0u
        && body->locals[1].parameter_index == 1u
        && body->locals[2].parameter_index == CM_HIR_PARAMETER_INDEX_NONE
        && body->locals[3].parameter_index == CM_HIR_PARAMETER_INDEX_NONE
        && body->locals[2].type == body->expected_type
        && body->locals[3].type == body->expected_type
        && body->locals[2].mutability == CM_HIR_IMMUTABLE
        && body->locals[3].mutability == CM_HIR_IMMUTABLE
        && hir_name_is(&fixture.hir, body->locals[2].name, "first")
        && hir_name_is(&fixture.hir, body->locals[3].name, "combined")
        && root != NULL && root->kind == CM_HIR_EXPR_BLOCK
        && root->data.block.statement_count == 2u
        && root->data.block.statements != NULL
        && fixture.hir.storage.active_marks.len == 0u
        && fixture.hir.strings.strings.active_marks.len == 0u);
    assert(root->data.block.statements[0].kind == CM_HIR_STATEMENT_LET
        && root->data.block.statements[0].data.let_statement.local_index == 2u
        && root->data.block.statements[1].kind == CM_HIR_STATEMENT_LET
        && root->data.block.statements[1].data.let_statement.local_index == 3u
        && root->data.block.statements[0].span.source == fixture.source
        && root->data.block.statements[1].span.source == fixture.source
        && root->data.block.statements[0].span.end
            <= root->data.block.statements[1].span.start);
    first = cm_hir_get_expr(&fixture.hir,
        root->data.block.statements[0].data.let_statement.initializer);
    first_left = first == NULL || first->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, first->data.binary.left);
    combined = cm_hir_get_expr(&fixture.hir,
        root->data.block.statements[1].data.let_statement.initializer);
    combined_first = combined == NULL || combined->kind != CM_HIR_EXPR_CALL
        ? NULL : cm_hir_get_expr(&fixture.hir,
            combined->data.call.arguments[0]);
    combined_right = combined == NULL || combined->kind != CM_HIR_EXPR_CALL
        ? NULL : cm_hir_get_expr(&fixture.hir,
            combined->data.call.arguments[1]);
    tail = cm_hir_get_expr(&fixture.hir,
        root->data.block.tail_expression);
    tail_combined = tail == NULL || tail->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, tail->data.binary.left);
    tail_right = tail == NULL || tail->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, tail->data.binary.right);
    tail_left = tail_right == NULL
        || tail_right->kind != CM_HIR_EXPR_BINARY ? NULL
        : cm_hir_get_expr(&fixture.hir, tail_right->data.binary.left);
    assert(first != NULL && first->kind == CM_HIR_EXPR_BINARY
        && first->data.binary.operator_kind == CM_HIR_BINARY_ADD
        && first_left != NULL && first_left->kind == CM_HIR_EXPR_LOCAL
        && first_left->data.local.local_index == 0u
        && combined != NULL && combined->kind == CM_HIR_EXPR_CALL
        && combined->data.call.argument_count == 2u
        && combined_first != NULL
        && combined_first->kind == CM_HIR_EXPR_LOCAL
        && combined_first->data.local.local_index == 2u
        && combined_right != NULL
        && combined_right->kind == CM_HIR_EXPR_BINARY
        && tail != NULL && tail->kind == CM_HIR_EXPR_BINARY
        && tail_combined != NULL
        && tail_combined->kind == CM_HIR_EXPR_LOCAL
        && tail_combined->data.local.local_index == 3u
        && tail_right != NULL && tail_right->kind == CM_HIR_EXPR_BINARY
        && tail_left != NULL && tail_left->kind == CM_HIR_EXPR_LOCAL
        && tail_left->data.local.local_index == 0u
        && fixture.hir.expressions.len == 14u);

    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, &fixture.hir) == 0);
    dump = read_dump(stream);
    assert(strncmp(dump, "hir-v27\n", strlen("hir-v27\n")) == 0
        && strstr(dump,
            "statements=[let(local=2,initializer=expr#3") != NULL
        && strstr(dump,
            ",let(local=3,initializer=expr#8") != NULL
        && strstr(dump,
            "body-local body#2 index=2 origin=local name=\"first\"") != NULL
        && strstr(dump,
            "body-local body#2 index=3 origin=local name=\"combined\"")
            != NULL);
    free(dump);
    assert(fclose(stream) == 0);
    fixture_destroy(&fixture);
}

static void test_graph_inferred_let_types(void)
{
    static const char default_source[] =
        "fn main() -> u32 { let unused = 0; 1u32 }";
    static const char propagation_source[] =
        "fn main() -> u32 { let x = 1i32; let y = x; 2u32 }";
    static const char return_context_source[] =
        "fn main() -> u32 { let x = 1; x }";
    static const char expression_source[] =
        "fn main(left: u32, right: u32) -> u32 { "
        "let sum = left + (right - 1); let copy = sum; copy }";
    TestFixture fixture;
    const CmHirBody *body;
    const CmHirExpr *root;
    const CmHirExpr *initializer;
    const CmHirExpr *tail;
    const CmHirType *type;
    const CmHirType *expression_type;
    CmHirBodyLowerResult result;
    size_t type_count;

    fixture_init(&fixture, default_source);
    type_count = fixture.hir.types.len;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    type = body == NULL || body->local_count != 1u ? NULL
        : cm_hir_get_type(&fixture.hir, body->locals[0].type);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.types.len == type_count + 1u
        && type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_I32);
    fixture_destroy(&fixture);

    fixture_init(&fixture, propagation_source);
    type_count = fixture.hir.types.len;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.types.len == type_count + 1u
        && body->local_count == 2u
        && body->locals[0].type == body->locals[1].type);
    type = cm_hir_get_type(&fixture.hir, body->locals[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_I32);
    fixture_destroy(&fixture);

    fixture_init(&fixture, return_context_source);
    type_count = fixture.hir.types.len;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    initializer = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
            || root->data.block.statement_count != 1u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.statements[0].data.let_statement.initializer);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    type = body == NULL || body->local_count != 1u ? NULL
        : cm_hir_get_type(&fixture.hir, body->locals[0].type);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.types.len == type_count
        && body->local_count == 1u
        && body->locals[0].type == body->expected_type
        && type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32
        && initializer != NULL && initializer->kind == CM_HIR_EXPR_INTEGER
        && initializer->type == body->expected_type
        && tail != NULL && tail->kind == CM_HIR_EXPR_LOCAL
        && tail->type == body->expected_type
        && tail->data.local.local_index == 0u);
    fixture_destroy(&fixture);

    fixture_init(&fixture, expression_source);
    type_count = fixture.hir.types.len;
    result = lower_fixture_body(&fixture);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    root = body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        body->root_expression);
    initializer = root == NULL || root->kind != CM_HIR_EXPR_BLOCK
            || root->data.block.statement_count != 2u
        ? NULL : cm_hir_get_expr(&fixture.hir,
            root->data.block.statements[0].data.let_statement.initializer);
    tail = root == NULL || root->kind != CM_HIR_EXPR_BLOCK ? NULL
        : cm_hir_get_expr(&fixture.hir, root->data.block.tail_expression);
    expression_type = body == NULL || body->local_count != 4u ? NULL
        : cm_hir_get_type(&fixture.hir, body->locals[2].type);
    assert(result.status == CM_HIR_BODY_LOWER_OK
        && body != NULL && body->state == CM_HIR_BODY_TYPED
        && fixture.hir.types.len == type_count
        && body->local_count == 4u
        && body->locals[2].type == body->locals[3].type
        && expression_type != NULL
        && expression_type->kind == CM_HIR_TYPE_INTEGER_KIND
        && expression_type->data.integer_type.kind == CM_HIR_INT_U32
        && initializer != NULL && initializer->kind == CM_HIR_EXPR_BINARY
        && initializer->data.binary.operator_kind == CM_HIR_BINARY_ADD
        && tail != NULL && tail->kind == CM_HIR_EXPR_LOCAL
        && tail->data.local.local_index == 3u);
    fixture_destroy(&fixture);
}

static void test_graph_inferred_let_failures_are_transactional(void)
{
    expect_body_failure(
        "fn main() -> u32 { let value = 1i32; value }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_body_failure(
        "fn identity(value: u32) -> u32 { value } "
        "fn main() -> u32 { let value = identity(1u32); value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let value = left * 1u32; value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let value = left + 1i32; value }",
        CM_HIR_BODY_LOWER_TYPE_MISMATCH);
    expect_body_failure(
        "fn main() -> u32 { let first = later; let later = 1u32; first }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
    expect_body_failure(
        "fn main(value: u32) -> u32 { let value = 1u32; value }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
    expect_body_failure(
        "fn main() -> u32 { let value = 1u32; let value = 2u32; value }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
}

static void test_graph_explicit_u32_let_rejections(void)
{
    expect_body_failure(
        "fn main(left: u32) -> u32 { let value: i32 = 1i32; left }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_TYPE);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let mut value: u32 = left; value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let ref value: u32 = left; value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let value @ _: u32 = left; value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let value: u32; left }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let value: u32 = left else { "
        "return 0u32; }; value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let left: u32 = 1u32; left }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let value: u32 = left; "
        "let value: u32 = 1u32; value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { #[rustfmt::skip] "
        "let value: u32 = left; value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { left + 1u32; left }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { use crate::left; left }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "struct Pair { left: u32 } "
        "fn main(left: u32) -> Pair { "
        "Pair { #[cfg(any())] left } }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_BODY);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let first: u32 = later; "
        "let later: u32 = left; first }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let value: u32 = value; value }",
        CM_HIR_BODY_LOWER_UNRESOLVED_PATH);
    expect_body_failure(
        "fn main(left: u32) -> u32 { let value: u32 = left * 1u32; value }",
        CM_HIR_BODY_LOWER_UNSUPPORTED_OPERATOR);
}

static void test_binary_model_ownership_and_copy(void)
{
    static const char source[] =
        "fn left(x: u32) -> u32 { x }\n"
        "fn add(x: u32, y: u32) -> u32 { x + y }\n";
    TestFixture fixture;
    const CmHirItem *left_item;
    const CmHirBody *left_body;
    const CmHirBody *add_body;
    const CmHirExpr *left_root;
    const CmHirExpr *foreign;
    const CmHirExpr *stored;
    CmHirBodyLowerResult result;
    CmHirExprId first;
    CmHirExprId second;
    CmHirExprId binary;
    CmHirExprId invalid;
    CmHirExprId saved_first;
    CmHirExprId saved_second;
    CmSpan binary_span;

    fixture_init_named(&fixture, source, "add");
    left_item = find_function(&fixture.hir, "left");
    assert(left_item != NULL);
    result = cm_hir_lower_body(&fixture.hir,
        left_item->data.function_item.body, &fixture.graph,
        fixture.graph_result.revision, &fixture.imports, &fixture.map);
    assert(result.status == CM_HIR_BODY_LOWER_OK);
    left_body = cm_hir_get_body(&fixture.hir,
        left_item->data.function_item.body);
    left_root = left_body == NULL ? NULL : cm_hir_get_expr(&fixture.hir,
        left_body->root_expression);
    foreign = left_root == NULL || left_root->kind != CM_HIR_EXPR_BLOCK
        ? NULL : cm_hir_get_expr(&fixture.hir,
            left_root->data.block.tail_expression);
    add_body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(foreign != NULL && add_body != NULL
        && add_body->state == CM_HIR_BODY_UNLOWERED
        && add_body->local_count == 2u);
    assert(cm_hir_body_add_local_expression(&fixture.hir, fixture.body, 0u,
        add_body->expected_type,
        (CmSpan){ fixture.source, add_body->span.start + 1u,
            add_body->span.start + 2u }, &first) == CM_HIR_OK);
    assert(cm_hir_body_add_local_expression(&fixture.hir, fixture.body, 1u,
        add_body->expected_type,
        (CmSpan){ fixture.source, add_body->span.start + 3u,
            add_body->span.start + 4u }, &second) == CM_HIR_OK);
    binary_span.source = fixture.source;
    binary_span.start = add_body->span.start;
    binary_span.end = add_body->span.end;
    invalid = 99u;
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        CM_HIR_BINARY_ADD, left_root->data.block.tail_expression,
        second, add_body->expected_type, binary_span, &invalid)
        == CM_HIR_INVARIANT_VIOLATION
        && invalid == CM_HIR_EXPR_NONE);
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        (CmHirBinaryOperator)99, first, second, add_body->expected_type,
        binary_span, &invalid) == CM_HIR_INVARIANT_VIOLATION);
    saved_first = first;
    saved_second = second;
    assert(cm_hir_body_add_binary_expression(&fixture.hir, fixture.body,
        CM_HIR_BINARY_ADD, first, second, add_body->expected_type,
        binary_span, &binary) == CM_HIR_OK);
    first = CM_HIR_EXPR_NONE;
    second = CM_HIR_EXPR_NONE;
    stored = cm_hir_get_expr(&fixture.hir, binary);
    assert(stored != NULL && stored->kind == CM_HIR_EXPR_BINARY
        && stored->data.binary.left == saved_first
        && stored->data.binary.right == saved_second);
    fixture_destroy(&fixture);
}

static void test_graph_u32_add_oom_is_transactional(void)
{
    TestFixture fixture;
    const CmHirBody *body;

    fixture_init_named(&fixture,
        "fn add(x: u32, y: u32) -> u32 { x + y }", "add");
    assert(fixture.hir.expressions.len == 0u
        && fixture.hir.expressions.cap == 0u);
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(oom_jump) == 0) {
        (void)lower_fixture_body(&fixture);
        assert(0 && "binary expression reservation unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    body = cm_hir_get_body(&fixture.hir, fixture.body);
    assert(body != NULL && body->state == CM_HIR_BODY_UNLOWERED
        && body->root_expression == CM_HIR_EXPR_NONE
        && fixture.hir.expressions.len == 0u
        && fixture.hir.storage.active_marks.len == 0u);
    fixture_destroy(&fixture);
}

static void expect_graph_mutable_static_body_rejected(const char *source)
{
    CmSourceSet sources;
    CmSourceId source_id;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;

    cm_source_set_init(&sources);
    assert(cm_source_add_memory(&sources, "value/lib.rs",
        (const unsigned char *)source, strlen(source), &source_id)
        == CM_SOURCE_OK);
    cm_cfg_set_init(&cfg);
    cm_module_graph_init(&graph);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, source_id,
        &graph_options);
    assert(graph_result.error_count == 0u);
    cm_import_resolver_init(&imports);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.error_count == 0u);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "value_body";
    lower_result = cm_hir_lower_module_graph(&hir, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 1u
        && lower_result.first_error.kind == CM_HIR_LOWER_UNSUPPORTED_ITEM
        && hir.crates.len == 0u && hir.modules.len == 0u
        && hir.items.len == 0u && hir.bodies.len == 0u
        && hir.expressions.len == 0u
        && cm_hir_module_map_count(&map) == 0u);
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static CmHirTypeId add_integer_type(CmHirContext *hir, CmHirIntType kind,
    CmSpan span)
{
    CmHirType type;
    CmHirTypeId type_id;

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = span;
    type.data.integer_type.kind = kind;
    assert(cm_hir_add_type(hir, &type, &type_id) == CM_HIR_OK);
    return type_id;
}

static void test_owned_local_and_instantiated_call_model(void)
{
    CmHirContext hir;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirTypeId u32_type;
    CmHirTypeId u8_type;
    CmHirDefId identity_definition;
    CmHirGenericParam generic_parameter;
    CmHirGenericParamId generic_parameter_id;
    CmHirType parameter_type_value;
    CmHirTypeId parameter_type;
    CmHirFunctionParameter identity_parameter;
    CmHirLocal identity_local;
    CmHirBody identity_body_value;
    CmHirBodyId identity_body;
    CmHirItem identity_item;
    CmHirItemId identity_item_id;
    CmHirExprId identity_root;
    CmHirDefId probe_definition;
    CmHirFunctionParameter probe_parameter;
    CmHirLocal probe_local;
    CmHirBody probe_body_value;
    CmHirBodyId probe_body;
    CmHirItem probe_item;
    CmHirItemId probe_item_id;
    CmHirExprId probe_argument;
    CmHirExprId probe_call;
    CmHirExprId invalid_expression;
    CmHirTypeId substitutions[1];
    CmHirExprId arguments[1];
    const CmHirExpr *stored_call;
    size_t expression_count;
    size_t body_count;
    uint64_t semantic_generation;
    FILE *stream;
    char *dump;

    cm_hir_context_init(&hir);
    assert(cm_hir_create_crate(&hir, cm_hir_intern(&hir, "call_model"),
        CM_HIR_EDITION_2021, (CmSpan){ 1u, 0u, 200u }, &crate_id,
        &root_module) == CM_HIR_OK);
    u32_type = add_integer_type(&hir, CM_HIR_INT_U32,
        (CmSpan){ 1u, 1u, 4u });
    u8_type = add_integer_type(&hir, CM_HIR_INT_U8,
        (CmSpan){ 1u, 5u, 7u });

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, (CmSpan){ 1u, 10u, 70u },
        &identity_definition) == CM_HIR_OK);
    memset(&generic_parameter, 0, sizeof(generic_parameter));
    generic_parameter.kind = CM_HIR_GENERIC_TYPE;
    generic_parameter.owner = identity_definition;
    generic_parameter.index = 0u;
    generic_parameter.name = cm_hir_intern(&hir, "T");
    generic_parameter.span = (CmSpan){ 1u, 22u, 23u };
    assert(cm_hir_add_generic_param(&hir, &generic_parameter,
        &generic_parameter_id) == CM_HIR_OK);
    memset(&parameter_type_value, 0, sizeof(parameter_type_value));
    parameter_type_value.kind = CM_HIR_TYPE_PARAMETER_KIND;
    parameter_type_value.span = generic_parameter.span;
    parameter_type_value.data.parameter_type.parameter = generic_parameter_id;
    assert(cm_hir_add_type(&hir, &parameter_type_value, &parameter_type)
        == CM_HIR_OK);
    memset(&identity_parameter, 0, sizeof(identity_parameter));
    identity_parameter.name = cm_hir_intern(&hir, "x");
    identity_parameter.type = parameter_type;
    identity_parameter.span = (CmSpan){ 1u, 25u, 29u };
    identity_parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&identity_local, 0, sizeof(identity_local));
    identity_local.name = identity_parameter.name;
    identity_local.type = parameter_type;
    identity_local.span = identity_parameter.span;
    identity_local.parameter_index = 0u;
    memset(&identity_body_value, 0, sizeof(identity_body_value));
    identity_body_value.owner = identity_definition;
    identity_body_value.state = CM_HIR_BODY_UNLOWERED;
    identity_body_value.expected_type = parameter_type;
    identity_body_value.locals = &identity_local;
    identity_body_value.local_count = 1u;
    identity_body_value.parameter_count = 1u;
    identity_body_value.source = 1u;
    identity_body_value.source_expression_id = 1u;
    identity_body_value.span = (CmSpan){ 1u, 10u, 70u };
    assert(cm_hir_add_body(&hir, &identity_body_value, &identity_body)
        == CM_HIR_OK);
    memset(&identity_item, 0, sizeof(identity_item));
    identity_item.kind = CM_HIR_ITEM_FUNCTION;
    identity_item.definition = identity_definition;
    identity_item.owner_module = root_module;
    identity_item.parent_definition = cm_hir_def_id_none();
    identity_item.name = cm_hir_intern(&hir, "identity");
    identity_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    identity_item.visibility.restriction = cm_hir_def_id_none();
    identity_item.span = identity_body_value.span;
    identity_item.generic_parameter_start = generic_parameter_id;
    identity_item.generic_parameter_count = 1u;
    identity_item.data.function_item.signature.parameters =
        &identity_parameter;
    identity_item.data.function_item.signature.parameter_count = 1u;
    identity_item.data.function_item.signature.return_type = parameter_type;
    identity_item.data.function_item.signature.abi =
        cm_hir_intern(&hir, "Rust");
    identity_item.data.function_item.signature.safety = CM_HIR_SAFE;
    identity_item.data.function_item.body = identity_body;
    identity_item.data.function_item.trait_item_definition =
        cm_hir_def_id_none();
    assert(cm_hir_add_item(&hir, &identity_item, &identity_item_id)
        == CM_HIR_OK);
    assert(cm_hir_body_add_local_expression(&hir, identity_body, 0u,
        parameter_type, (CmSpan){ 1u, 50u, 51u }, &identity_root)
        == CM_HIR_OK);
    expression_count = hir.expressions.len;
    body_count = hir.bodies.len;
    semantic_generation = hir.semantic_generation;
    assert(cm_hir_set_body_root_expression(&hir, identity_body,
        identity_root) == CM_HIR_OK);
    assert(hir.expressions.len == expression_count
        && hir.bodies.len == body_count
        && hir.semantic_generation == semantic_generation + UINT64_C(1));

    assert(cm_hir_reserve_item_definition_as(&hir, crate_id,
        CM_HIR_ITEM_FUNCTION, (CmSpan){ 1u, 80u, 170u },
        &probe_definition) == CM_HIR_OK);
    memset(&probe_parameter, 0, sizeof(probe_parameter));
    probe_parameter.name = cm_hir_intern(&hir, "x");
    probe_parameter.type = u32_type;
    probe_parameter.span = (CmSpan){ 1u, 95u, 101u };
    probe_parameter.binding_kind = CM_HIR_BINDING_NAMED;
    memset(&probe_local, 0, sizeof(probe_local));
    probe_local.name = probe_parameter.name;
    probe_local.type = u32_type;
    probe_local.span = probe_parameter.span;
    probe_local.parameter_index = 0u;
    memset(&probe_body_value, 0, sizeof(probe_body_value));
    probe_body_value.owner = probe_definition;
    probe_body_value.state = CM_HIR_BODY_UNLOWERED;
    probe_body_value.expected_type = u32_type;
    probe_body_value.locals = &probe_local;
    probe_body_value.local_count = 1u;
    probe_body_value.parameter_count = 1u;
    probe_body_value.source = 1u;
    probe_body_value.source_expression_id = 2u;
    probe_body_value.span = (CmSpan){ 1u, 80u, 170u };
    assert(cm_hir_add_body(&hir, &probe_body_value, &probe_body)
        == CM_HIR_OK);
    memset(&probe_item, 0, sizeof(probe_item));
    probe_item.kind = CM_HIR_ITEM_FUNCTION;
    probe_item.definition = probe_definition;
    probe_item.owner_module = root_module;
    probe_item.parent_definition = cm_hir_def_id_none();
    probe_item.name = cm_hir_intern(&hir, "probe");
    probe_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    probe_item.visibility.restriction = cm_hir_def_id_none();
    probe_item.span = probe_body_value.span;
    probe_item.data.function_item.signature.parameters = &probe_parameter;
    probe_item.data.function_item.signature.parameter_count = 1u;
    probe_item.data.function_item.signature.return_type = u32_type;
    probe_item.data.function_item.signature.abi = cm_hir_intern(&hir, "Rust");
    probe_item.data.function_item.signature.safety = CM_HIR_SAFE;
    probe_item.data.function_item.body = probe_body;
    probe_item.data.function_item.trait_item_definition = cm_hir_def_id_none();
    assert(cm_hir_add_item(&hir, &probe_item, &probe_item_id) == CM_HIR_OK);

    invalid_expression = 99u;
    assert(cm_hir_body_add_local_expression(&hir, probe_body, 1u,
        u32_type, (CmSpan){ 1u, 130u, 131u }, &invalid_expression)
        == CM_HIR_INVARIANT_VIOLATION
        && invalid_expression == CM_HIR_EXPR_NONE);
    assert(cm_hir_body_add_local_expression(&hir, probe_body, 0u,
        u8_type, (CmSpan){ 1u, 130u, 131u }, &invalid_expression)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(cm_hir_body_add_local_expression(&hir, probe_body, 0u,
        u32_type, (CmSpan){ 2u, 130u, 131u }, &invalid_expression)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(cm_hir_body_add_local_expression(&hir, probe_body, 0u,
        u32_type, (CmSpan){ 1u, 130u, 131u }, &probe_argument)
        == CM_HIR_OK);

    substitutions[0] = u32_type;
    arguments[0] = identity_root;
    assert(cm_hir_body_add_call_expression(&hir, probe_body,
        identity_definition, substitutions, 1u, arguments, 1u, u32_type,
        (CmSpan){ 1u, 120u, 150u }, &invalid_expression)
        == CM_HIR_INVARIANT_VIOLATION);
    arguments[0] = probe_argument;
    substitutions[0] = u8_type;
    assert(cm_hir_body_add_call_expression(&hir, probe_body,
        identity_definition, substitutions, 1u, arguments, 1u, u32_type,
        (CmSpan){ 1u, 120u, 150u }, &invalid_expression)
        == CM_HIR_INVARIANT_VIOLATION);
    substitutions[0] = u32_type;
    assert(cm_hir_body_add_call_expression(&hir, probe_body,
        identity_definition, substitutions, 1u, arguments, 1u, u8_type,
        (CmSpan){ 1u, 120u, 150u }, &invalid_expression)
        == CM_HIR_INVARIANT_VIOLATION);
    assert(cm_hir_set_body_root_expression(&hir, probe_body, identity_root)
        == CM_HIR_INVARIANT_VIOLATION);

    expression_count = hir.expressions.len;
    cm_alloc_set_oom_handler(jump_on_oom, NULL);
    cm_alloc_fail_after(0u);
    if (setjmp(oom_jump) == 0) {
        (void)cm_hir_body_add_call_expression(&hir, probe_body,
            identity_definition, substitutions, 1u, arguments, 1u,
            u32_type, (CmSpan){ 1u, 120u, 150u }, &invalid_expression);
        assert(0 && "call payload allocation unexpectedly survived OOM");
    }
    cm_alloc_fail_never();
    cm_alloc_set_oom_handler(NULL, NULL);
    assert(hir.expressions.len == expression_count
        && hir.storage.active_marks.len == 0u
        && cm_hir_get_body(&hir, probe_body)->state
            == CM_HIR_BODY_UNLOWERED);

    assert(cm_hir_body_add_call_expression(&hir, probe_body,
        identity_definition, substitutions, 1u, arguments, 1u, u32_type,
        (CmSpan){ 1u, 120u, 150u }, &probe_call) == CM_HIR_OK);
    substitutions[0] = u8_type;
    arguments[0] = identity_root;
    stored_call = cm_hir_get_expr(&hir, probe_call);
    assert(stored_call != NULL && stored_call->kind == CM_HIR_EXPR_CALL
        && stored_call->owner_body == probe_body
        && cm_hir_def_id_equal(stored_call->data.call.callee,
            identity_definition)
        && stored_call->data.call.type_substitution_count == 1u
        && stored_call->data.call.type_substitutions[0] == u32_type
        && stored_call->data.call.argument_count == 1u
        && stored_call->data.call.arguments[0] == probe_argument);
    expression_count = hir.expressions.len;
    body_count = hir.bodies.len;
    semantic_generation = hir.semantic_generation;
    assert(cm_hir_set_body_root_expression(&hir, probe_body, probe_call)
        == CM_HIR_OK);
    assert(hir.expressions.len == expression_count
        && hir.bodies.len == body_count
        && hir.semantic_generation == semantic_generation + UINT64_C(1));

    stream = tmpfile();
    assert(stream != NULL && cm_hir_dump(stream, &hir) == 0);
    dump = read_dump(stream);
    assert(strstr(dump,
        "expr#1 local type=ty#3 local=0 owner=body#1") != NULL);
    assert(strstr(dump,
        "expr#2 local type=ty#1 local=0 owner=body#2") != NULL);
    assert(strstr(dump,
        "expr#3 call type=ty#1 callee=1:2 substitutions=[ty#1] "
        "arguments=[expr#2] owner=body#2") != NULL);
    free(dump);
    assert(fclose(stream) == 0);
    cm_hir_context_destroy(&hir);
}

int main(void)
{
    test_all_local_bodies_transaction();
    test_free_value_body_owners();
    test_value_body_atomic_rollback_and_owner_rejection();
    test_value_block_rejections_are_transactional();
    test_closed_trait_default_bodies();
    test_generic_impl_method_body_owner();
    test_exact_i32_body();
    test_source_backed_named_aggregate_body();
    test_nested_and_empty_named_aggregate_bodies();
    test_imported_alias_named_aggregate_body();
    test_imported_parameter_named_field_projection();
    test_constructed_nested_named_field_projection();
    test_rejections_are_transactional();
    test_raw_reference_rejections_are_transactional();
    test_aggregate_resolver_snapshot_is_transactional();
    test_named_field_projection_rejections_are_transactional();
    test_oom_precedes_body_mutation();
    test_aggregate_payload_oom_is_transactional();
    test_graph_function_body();
    test_graph_explicit_local_ufcs_call();
    test_graph_unresolved_local_method_call();
    test_graph_method_call_oom_is_transactional();
    test_graph_identity_call_bodies();
    test_graph_generic_identity_call_body();
    test_graph_generic_identity_call_rejects_wrong_parameter();
    test_graph_identity_call_recursive_argument();
    test_graph_monomorphic_call_computed_arguments();
    test_graph_contextual_unsuffixed_call_argument();
    test_graph_monomorphic_aggregate_call_arguments();
    test_graph_nested_monomorphic_call_expression();
    test_graph_call_nested_under_add_expression();
    test_graph_monomorphic_one_argument_call();
    test_graph_identity_call_rejections();
    test_graph_monomorphic_call_rejections();
    test_graph_nested_call_cycle_is_transactional();
    test_graph_identity_call_argument_graph_is_transactional();
    test_graph_identity_call_oom_is_transactional();
    test_graph_monomorphic_call_oom_is_transactional();
    test_graph_u32_add_body();
    test_graph_nested_u32_add_body();
    test_graph_u32_subtract_body();
    test_graph_u32_equal_if_else_body();
    test_graph_u32_equal_if_reuses_bool_type();
    test_graph_u32_equal_if_rejections();
    test_u32_equal_if_model_invariants();
    test_graph_u32_equal_if_oom_is_transactional();
    test_graph_usize_scalar_body();
    test_graph_usize_less_if_else_body();
    test_graph_usize_rejections();
    test_usize_binary_model_invariants();
    test_graph_usize_less_if_oom_is_transactional();
    test_graph_u32_add_rejections();
    test_graph_explicit_u32_let_block();
    test_graph_inferred_let_types();
    test_graph_inferred_let_failures_are_transactional();
    test_graph_explicit_u32_let_rejections();
    test_binary_model_ownership_and_copy();
    test_graph_u32_add_oom_is_transactional();
    test_owned_local_and_instantiated_call_model();
    expect_graph_mutable_static_body_rejected(
        "static mut VALUE: i32 = 7i32;");
    puts("hir-body: ok");
    return 0;
}
