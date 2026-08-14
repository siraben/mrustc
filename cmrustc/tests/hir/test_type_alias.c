#include "cm/hir/lower.h"
#include "cm/hir/type_alias.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "hir-type-alias: %s\n", message);
        failures += 1;
    }
}

static int hir_name_is(const CmHirContext *hir, CmInternId id,
    const char *expected)
{
    const CmInternedString *name;
    size_t length;

    name = cm_interner_get(&hir->strings, id);
    length = strlen(expected);
    return name != NULL && name->len == length
        && memcmp(name->bytes, expected, length) == 0;
}

static const CmHirItem *find_item(const CmHirContext *hir,
    const char *name)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item != NULL && hir_name_is(hir, item->name, name)) {
            return item;
        }
    }
    return NULL;
}

static const CmHirGenericParam *find_generic(const CmHirContext *hir,
    CmHirDefId owner, CmHirGenericParamKind kind)
{
    size_t index;

    for (index = 0u; index < hir->generic_parameters.len; ++index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &hir->generic_parameters, index);
        if (parameter != NULL && cm_hir_def_id_equal(parameter->owner, owner)
            && parameter->kind == kind) {
            return parameter;
        }
    }
    return NULL;
}

static const CmHirGenericParam *find_generic_at(const CmHirContext *hir,
    CmHirDefId owner, uint32_t parameter_index)
{
    size_t index;

    for (index = 0u; index < hir->generic_parameters.len; ++index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &hir->generic_parameters, index);
        if (parameter != NULL && cm_hir_def_id_equal(parameter->owner, owner)
            && parameter->index == parameter_index) {
            return parameter;
        }
    }
    return NULL;
}

static CmHirGenericParamId find_generic_id(const CmHirContext *hir,
    const CmHirGenericParam *parameter)
{
    size_t index;

    if (parameter == NULL) return CM_HIR_GENERIC_PARAM_NONE;
    for (index = 0u; index < hir->generic_parameters.len; ++index) {
        if (cm_vec_at_const(&hir->generic_parameters, index) == parameter) {
            return (CmHirGenericParamId)(index + 1u);
        }
    }
    return CM_HIR_GENERIC_PARAM_NONE;
}

static int build_memory_graph(const char *source, CmSourceSet *sources,
    CmModuleGraph *graph, CmCfgSet *cfg, CmImportResolver *imports,
    CmModuleGraphResult *graph_result, CmImportResult *import_result)
{
    CmSourceId root;
    CmModuleGraphOptions options;

    cm_source_set_init(sources);
    cm_module_graph_init(graph);
    cm_cfg_set_init(cfg);
    cm_import_resolver_init(imports);
    if (cm_source_add_memory(sources, "type-alias/lib.rs",
            (const unsigned char *)source, strlen(source), &root)
        != CM_SOURCE_OK) {
        return 0;
    }
    cm_module_graph_options_init(&options);
    options.cfg = cfg;
    *graph_result = cm_module_graph_build(graph, sources, root, &options);
    *import_result = cm_import_resolve(imports, graph,
        graph_result->revision);
    return graph_result->error_count == 0u
        && import_result->error_count == 0u;
}

static int build_file_graph(const char *path, CmSourceSet *sources,
    CmModuleGraph *graph, CmCfgSet *cfg, CmImportResolver *imports,
    CmModuleGraphResult *graph_result, CmImportResult *import_result)
{
    CmSourceId root;
    CmModuleGraphOptions options;

    cm_source_set_init(sources);
    cm_module_graph_init(graph);
    cm_cfg_set_init(cfg);
    cm_import_resolver_init(imports);
    if (cm_source_load_file(sources, path, &root) != CM_SOURCE_OK) {
        return 0;
    }
    cm_module_graph_options_init(&options);
    options.cfg = cfg;
    *graph_result = cm_module_graph_build(graph, sources, root, &options);
    *import_result = cm_import_resolve(imports, graph,
        graph_result->revision);
    return graph_result->error_count == 0u
        && import_result->error_count == 0u;
}

static CmHirLowerResult lower_graph(CmHirContext *hir,
    const CmModuleGraph *graph, CmModuleGraphRevision revision,
    const CmImportResolver *imports, CmHirModuleMap *map)
{
    CmHirLowerOptions options;

    cm_hir_lower_options_init(&options);
    options.crate_name = "type_alias_test";
    return cm_hir_lower_module_graph(hir, graph, revision, imports, map,
        &options);
}

static void destroy_graph(CmSourceSet *sources, CmModuleGraph *graph,
    CmImportResolver *imports)
{
    cm_import_resolver_destroy(imports);
    cm_module_graph_destroy(graph);
    cm_source_set_destroy(sources);
}

static int type_is_integer(const CmHirContext *hir, CmHirTypeId id,
    CmHirIntType kind)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == kind;
}

static int type_is_adt(const CmHirContext *hir, CmHirTypeId id,
    CmHirDefId definition)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition, definition)
        && type->data.named_type.argument_count == 0u;
}

static int parameter_type_is(const CmHirContext *hir, CmHirTypeId id,
    CmHirGenericParamId parameter)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, id);
    return type != NULL && type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && type->data.parameter_type.parameter == parameter;
}

static int pair_type_matches_integer(const CmHirContext *hir,
    CmHirTypeId id, CmHirIntType kind)
{
    const CmHirType *tuple;
    const CmHirType *pointer;

    tuple = cm_hir_get_type(hir, id);
    if (tuple == NULL || tuple->kind != CM_HIR_TYPE_TUPLE_KIND
        || tuple->data.tuple_type.element_count != 2u) {
        return 0;
    }
    pointer = cm_hir_get_type(hir, tuple->data.tuple_type.elements[1]);
    return type_is_integer(hir, tuple->data.tuple_type.elements[0], kind)
        && pointer != NULL && pointer->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && pointer->data.raw_pointer_type.mutability == CM_HIR_IMMUTABLE
        && type_is_integer(hir, pointer->data.raw_pointer_type.pointee, kind);
}

static int pair_type_matches_adt(const CmHirContext *hir, CmHirTypeId id,
    CmHirDefId definition)
{
    const CmHirType *tuple;
    const CmHirType *pointer;

    tuple = cm_hir_get_type(hir, id);
    if (tuple == NULL || tuple->kind != CM_HIR_TYPE_TUPLE_KIND
        || tuple->data.tuple_type.element_count != 2u) {
        return 0;
    }
    pointer = cm_hir_get_type(hir, tuple->data.tuple_type.elements[1]);
    return type_is_adt(hir, tuple->data.tuple_type.elements[0], definition)
        && pointer != NULL && pointer->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && pointer->data.raw_pointer_type.mutability == CM_HIR_IMMUTABLE
        && type_is_adt(hir, pointer->data.raw_pointer_type.pointee,
            definition);
}

static int tuple_type_matches_integers(const CmHirContext *hir,
    CmHirTypeId id, CmHirIntType first, CmHirIntType second)
{
    const CmHirType *tuple;

    tuple = cm_hir_get_type(hir, id);
    return tuple != NULL && tuple->kind == CM_HIR_TYPE_TUPLE_KIND
        && tuple->data.tuple_type.element_count == 2u
        && type_is_integer(hir, tuple->data.tuple_type.elements[0], first)
        && type_is_integer(hir, tuple->data.tuple_type.elements[1], second);
}

static int lifetime_default_type_matches(const CmHirContext *hir,
    CmHirTypeId id, CmHirRegionKind region_kind)
{
    const CmHirType *tuple;
    const CmHirType *reference;
    const CmHirType *pointer;

    tuple = cm_hir_get_type(hir, id);
    if (tuple == NULL || tuple->kind != CM_HIR_TYPE_TUPLE_KIND
        || tuple->data.tuple_type.element_count != 2u) {
        return 0;
    }
    reference = cm_hir_get_type(hir, tuple->data.tuple_type.elements[0]);
    pointer = cm_hir_get_type(hir, tuple->data.tuple_type.elements[1]);
    return reference != NULL
        && reference->kind == CM_HIR_TYPE_REFERENCE_KIND
        && reference->data.reference_type.region.kind == region_kind
        && type_is_integer(hir, reference->data.reference_type.pointee,
            CM_HIR_INT_U16)
        && pointer != NULL && pointer->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && type_is_integer(hir, pointer->data.raw_pointer_type.pointee,
            CM_HIR_INT_U16);
}

static int reference_type_matches_integer(const CmHirContext *hir,
    CmHirTypeId id, CmHirRegionKind region_kind, CmHirIntType integer_kind)
{
    const CmHirType *reference;

    reference = cm_hir_get_type(hir, id);
    return reference != NULL && reference->kind == CM_HIR_TYPE_REFERENCE_KIND
        && reference->data.reference_type.region.kind == region_kind
        && type_is_integer(hir, reference->data.reference_type.pointee,
            integer_kind);
}

static int type_root_is_alias_free(const CmHirContext *hir,
    CmHirTypeId id, size_t depth);

static int named_type_is_alias_free(const CmHirContext *hir,
    const CmHirNamedType *named, size_t depth)
{
    uint32_t index;

    if (named->argument_count != 0u && named->arguments == NULL) return 0;
    for (index = 0u; index < named->argument_count; ++index) {
        const CmHirGenericArg *argument;

        argument = &named->arguments[index];
        if (argument->kind == CM_HIR_GENERIC_ARG_TYPE
            && !type_root_is_alias_free(hir, argument->data.type,
                depth + 1u)) {
            return 0;
        }
        if (argument->kind == CM_HIR_GENERIC_ARG_CONST
            && !type_root_is_alias_free(hir, argument->data.constant.type,
                depth + 1u)) {
            return 0;
        }
    }
    return 1;
}

static int type_root_is_alias_free(const CmHirContext *hir,
    CmHirTypeId id, size_t depth)
{
    const CmHirType *type;
    uint32_t index;

    if (id == CM_HIR_TYPE_NONE) return 1;
    if (depth > hir->types.len) return 0;
    type = cm_hir_get_type(hir, id);
    if (type == NULL || type->kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND) {
        return 0;
    }
    switch (type->kind) {
    case CM_HIR_TYPE_REFERENCE_KIND:
        return type_root_is_alias_free(hir,
            type->data.reference_type.pointee, depth + 1u);
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        return type_root_is_alias_free(hir,
            type->data.raw_pointer_type.pointee, depth + 1u);
    case CM_HIR_TYPE_TUPLE_KIND:
        if (type->data.tuple_type.element_count != 0u
            && type->data.tuple_type.elements == NULL) {
            return 0;
        }
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if (!type_root_is_alias_free(hir,
                    type->data.tuple_type.elements[index], depth + 1u)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_TYPE_ARRAY_KIND:
        return type_root_is_alias_free(hir,
                type->data.array_type.element, depth + 1u)
            && type_root_is_alias_free(hir,
                type->data.array_type.length.type, depth + 1u);
    case CM_HIR_TYPE_SLICE_KIND:
        return type_root_is_alias_free(hir,
            type->data.slice_type.element, depth + 1u);
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (type->data.fn_pointer_type.parameter_count != 0u
            && type->data.fn_pointer_type.parameters == NULL) {
            return 0;
        }
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (!type_root_is_alias_free(hir,
                    type->data.fn_pointer_type.parameters[index],
                    depth + 1u)) {
                return 0;
            }
        }
        return type_root_is_alias_free(hir,
            type->data.fn_pointer_type.return_type, depth + 1u);
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
    case CM_HIR_TYPE_ADT_KIND:
    case CM_HIR_TYPE_OPAQUE_KIND:
    case CM_HIR_TYPE_FOREIGN_KIND:
        return named_type_is_alias_free(hir, &type->data.named_type, depth);
    case CM_HIR_TYPE_CLOSURE_KIND:
        return cm_hir_get_closure(hir,
            type->data.closure_type.closure) != NULL;
    case CM_HIR_TYPE_PROJECTION_KIND:
        return type_root_is_alias_free(hir,
                type->data.projection_type.self_type, depth + 1u)
            && named_type_is_alias_free(hir,
                &type->data.projection_type.trait_type, depth)
            && named_type_is_alias_free(hir,
                &type->data.projection_type.associated_type, depth);
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        if (type->data.dyn_trait_type.has_principal
            && !named_type_is_alias_free(hir,
                &type->data.dyn_trait_type.principal_trait, depth)) {
            return 0;
        }
        for (index = 0u;
             index < type->data.dyn_trait_type.auto_trait_count; ++index) {
            if (!named_type_is_alias_free(hir,
                    &type->data.dyn_trait_type.auto_traits[index], depth)) {
                return 0;
            }
        }
        return 1;
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
        return 0;
    case CM_HIR_TYPE_ERROR_KIND:
    case CM_HIR_TYPE_INFER_KIND:
    case CM_HIR_TYPE_NEVER_KIND:
    case CM_HIR_TYPE_UNIT_KIND:
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
    case CM_HIR_TYPE_STR_KIND:
    case CM_HIR_TYPE_INTEGER_KIND:
    case CM_HIR_TYPE_FLOAT_KIND:
    case CM_HIR_TYPE_SELF_KIND:
    case CM_HIR_TYPE_PARAMETER_KIND:
        return 1;
    }
    return 0;
}

static int semantic_roots_are_alias_free(const CmHirContext *hir)
{
    size_t index;

    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        uint32_t first;
        uint32_t second;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        if (item == NULL) return 0;
        switch (item->kind) {
        case CM_HIR_ITEM_FUNCTION:
            for (first = 0u;
                 first < item->data.function_item.signature.parameter_count;
                 ++first) {
                if (!type_root_is_alias_free(hir,
                        item->data.function_item.signature
                            .parameters[first].type, 0u)) {
                    return 0;
                }
            }
            if (!type_root_is_alias_free(hir,
                    item->data.function_item.signature.return_type, 0u)) {
                return 0;
            }
            break;
        case CM_HIR_ITEM_STRUCT:
        case CM_HIR_ITEM_UNION:
            for (first = 0u;
                 first < item->data.aggregate_item.field_count; ++first) {
                if (!type_root_is_alias_free(hir,
                        item->data.aggregate_item.fields[first].type, 0u)) {
                    return 0;
                }
            }
            break;
        case CM_HIR_ITEM_ENUM:
            for (first = 0u; first < item->data.enum_item.variant_count;
                 ++first) {
                const CmHirVariant *variant;

                variant = &item->data.enum_item.variants[first];
                for (second = 0u; second < variant->field_count; ++second) {
                    if (!type_root_is_alias_free(hir,
                            variant->fields[second].type, 0u)) {
                        return 0;
                    }
                }
                if (variant->has_discriminant
                    && !type_root_is_alias_free(hir,
                        variant->discriminant.type, 0u)) {
                    return 0;
                }
            }
            break;
        case CM_HIR_ITEM_TYPE_ALIAS:
            if (!type_root_is_alias_free(hir,
                    item->data.type_alias_item.target, 0u)) {
                return 0;
            }
            break;
        case CM_HIR_ITEM_CONST:
        case CM_HIR_ITEM_STATIC:
            if (!type_root_is_alias_free(hir,
                    item->data.value_item.type, 0u)) {
                return 0;
            }
            break;
        case CM_HIR_ITEM_IMPL:
            if (!type_root_is_alias_free(hir,
                    item->data.impl_item.self_type, 0u)
                || (item->data.impl_item.has_trait
                    && !named_type_is_alias_free(hir,
                        &item->data.impl_item.trait_type, 0u))) {
                return 0;
            }
            break;
        case CM_HIR_ITEM_MODULE:
        case CM_HIR_ITEM_TRAIT:
        case CM_HIR_ITEM_TRAIT_ALIAS:
        case CM_HIR_ITEM_EXTERN_TYPE:
            break;
        }
    }
    for (index = 0u; index < hir->bodies.len; ++index) {
        const CmHirBody *body;
        uint32_t local;

        body = (const CmHirBody *)cm_vec_at_const(&hir->bodies, index);
        if (body == NULL
            || !type_root_is_alias_free(hir, body->expected_type, 0u)) {
            return 0;
        }
        for (local = 0u; local < body->local_count; ++local) {
            if (!type_root_is_alias_free(hir, body->locals[local].type,
                    0u)) {
                return 0;
            }
        }
    }
    for (index = 0u; index < hir->generic_parameters.len; ++index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &hir->generic_parameters, index);
        if (parameter == NULL
            || !type_root_is_alias_free(hir, parameter->declared_type, 0u)) {
            return 0;
        }
        if (parameter->has_default
            && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE
            && !type_root_is_alias_free(hir,
                parameter->default_argument.data.type, 0u)) {
            return 0;
        }
        if (parameter->has_default
            && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_CONST
            && !type_root_is_alias_free(hir,
                parameter->default_argument.data.constant.type, 0u)) {
            return 0;
        }
    }
    return 1;
}

static void test_structural_aliases(void)
{
    static const char source[] =
        "type Word = u32;"
        "type Pair<T> = (T, *const T);"
        "type Ref<'a, T> = &'a T;"
        "type Forward<T> = Later<T>;"
        "type Later<T> = Pair<T>;"
        "type Id<T> = T;"
        "type Callback<T> = fn(*const T) -> bool;"
        "struct Marker;"
        "struct Uses<'s> {"
        " primitive: Word,"
        " pair: Pair<u8>,"
        " borrowed: Ref<'s, Marker>,"
        " inferred: Ref<Marker>,"
        " placeholder: Ref<'_, Marker>,"
        " forward: Forward<u16>,"
        " nested: Id<Id<Marker>>,"
        " callback: Callback<u8>"
        "}";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmImportResolver imports;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerResult result;
    const CmHirItem *uses;
    const CmHirItem *marker;
    const CmHirItem *forward;
    const CmHirGenericParam *uses_lifetime;
    const CmHirGenericParam *forward_type;
    CmHirGenericParamId uses_lifetime_id;
    CmHirGenericParamId forward_type_id;
    const CmHirType *type;
    const CmHirType *pointee;
    const CmHirType *parameter;

    if (!build_memory_graph(source, &sources, &graph, &cfg, &imports,
            &graph_result, &import_result)) {
        check(0, "could not build structural alias graph");
        return;
    }
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    result = lower_graph(&hir, &graph, graph_result.revision, &imports, &map);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-type-alias: structural lowering: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    check(result.error_count == 0u,
        "structural aliases did not lower successfully");
    if (result.error_count != 0u) goto cleanup;

    uses = find_item(&hir, "Uses");
    marker = find_item(&hir, "Marker");
    forward = find_item(&hir, "Forward");
    check(uses != NULL && uses->kind == CM_HIR_ITEM_STRUCT
        && uses->data.aggregate_item.field_count == 8u
        && marker != NULL && marker->kind == CM_HIR_ITEM_STRUCT
        && forward != NULL && forward->kind == CM_HIR_ITEM_TYPE_ALIAS,
        "structural alias items or fields are missing");
    if (uses == NULL || uses->kind != CM_HIR_ITEM_STRUCT
        || uses->data.aggregate_item.field_count != 8u || marker == NULL
        || forward == NULL) {
        goto cleanup;
    }

    check(type_is_integer(&hir,
            uses->data.aggregate_item.fields[0].type, CM_HIR_INT_U32),
        "primitive alias did not normalize to u32");
    check(pair_type_matches_integer(&hir,
            uses->data.aggregate_item.fields[1].type, CM_HIR_INT_U8),
        "Pair<u8> did not substitute both occurrences of T");

    uses_lifetime = find_generic(&hir, uses->definition,
        CM_HIR_GENERIC_LIFETIME);
    uses_lifetime_id = find_generic_id(&hir, uses_lifetime);
    type = cm_hir_get_type(&hir, uses->data.aggregate_item.fields[2].type);
    check(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.region.kind == CM_HIR_REGION_EARLY_BOUND
        && type->data.reference_type.region.data.parameter
            == uses_lifetime_id
        && type_is_adt(&hir, type->data.reference_type.pointee,
            marker->definition),
        "Ref<'s, Marker> did not substitute lifetime and type parameters");
    type = cm_hir_get_type(&hir, uses->data.aggregate_item.fields[3].type);
    check(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.region.kind == CM_HIR_REGION_INFER
        && type_is_adt(&hir, type->data.reference_type.pointee,
            marker->definition),
        "Ref<Marker> did not infer its omitted lifetime");
    type = cm_hir_get_type(&hir, uses->data.aggregate_item.fields[4].type);
    check(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.region.kind == CM_HIR_REGION_INFER
        && type_is_adt(&hir, type->data.reference_type.pointee,
            marker->definition),
        "Ref<'_, Marker> did not create an inferred lifetime");

    check(pair_type_matches_integer(&hir,
            uses->data.aggregate_item.fields[5].type, CM_HIR_INT_U16),
        "forward alias chain did not normalize structurally");
    check(type_is_adt(&hir, uses->data.aggregate_item.fields[6].type,
            marker->definition),
        "nested Id<Id<Marker>> was rejected or left unexpanded");

    type = cm_hir_get_type(&hir, uses->data.aggregate_item.fields[7].type);
    check(type != NULL && type->kind == CM_HIR_TYPE_FN_POINTER_KIND
        && type->data.fn_pointer_type.parameter_count == 1u
        && type_is_integer(&hir, type->data.fn_pointer_type.return_type,
            CM_HIR_INT_U8) == 0,
        "Callback<u8> did not normalize to a function pointer");
    if (type != NULL && type->kind == CM_HIR_TYPE_FN_POINTER_KIND
        && type->data.fn_pointer_type.parameter_count == 1u) {
        parameter = cm_hir_get_type(&hir,
            type->data.fn_pointer_type.parameters[0]);
        check(parameter != NULL
            && parameter->kind == CM_HIR_TYPE_RAW_POINTER_KIND
            && type_is_integer(&hir,
                parameter->data.raw_pointer_type.pointee, CM_HIR_INT_U8),
            "Callback<u8> did not substitute its parameter type");
        pointee = cm_hir_get_type(&hir,
            type->data.fn_pointer_type.return_type);
        check(pointee != NULL && pointee->kind == CM_HIR_TYPE_BOOL_KIND,
            "Callback<u8> did not preserve its bool return type");
    }

    forward_type = find_generic(&hir, forward->definition,
        CM_HIR_GENERIC_TYPE);
    forward_type_id = find_generic_id(&hir, forward_type);
    type = cm_hir_get_type(&hir, forward->data.type_alias_item.target);
    check(type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count == 2u
        && parameter_type_is(&hir, type->data.tuple_type.elements[0],
            forward_type_id),
        "forward alias item target retained an alias application");
    if (type != NULL && type->kind == CM_HIR_TYPE_TUPLE_KIND
        && type->data.tuple_type.element_count == 2u) {
        pointee = cm_hir_get_type(&hir, type->data.tuple_type.elements[1]);
        check(pointee != NULL && pointee->kind == CM_HIR_TYPE_RAW_POINTER_KIND
            && parameter_type_is(&hir,
                pointee->data.raw_pointer_type.pointee, forward_type_id),
            "forward alias target did not substitute nested T");
    }
    check(semantic_roots_are_alias_free(&hir),
        "a semantic root retained a transient alias application");

cleanup:
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    destroy_graph(&sources, &graph, &imports);
}

static void test_trailing_type_defaults(void)
{
    static const char source[] =
        "type A<T = u8> = T;"
        "type B<T, U = T> = (T, U);"
        "type C<T = u8, U = T> = (T, U);"
        "type Forward<T = Marker> = T;"
        "type Nested<T = A> = T;"
        "type WithLifetime<'a, T = u16> = (&'a T, *const T);"
        "type Borrowed<'a, T = &'a u8> = T;"
        "struct Marker;"
        "struct Defaults<'s> {"
        " a: A,"
        " a_override: A<u16>,"
        " b: B<u32>,"
        " b_override: B<u32, u16>,"
        " c: C,"
        " c_partial: C<u16>,"
        " forward: Forward,"
        " nested: Nested,"
        " with_lifetime: WithLifetime<'s>,"
        " with_inferred_lifetime: WithLifetime,"
        " borrowed: Borrowed<'s>,"
        " borrowed_inferred: Borrowed"
        "}";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmImportResolver imports;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerResult result;
    const CmHirItem *a;
    const CmHirItem *b;
    const CmHirItem *forward;
    const CmHirItem *nested;
    const CmHirItem *marker;
    const CmHirItem *defaults;
    const CmHirGenericParam *parameter;
    const CmHirType *default_type;

    if (!build_memory_graph(source, &sources, &graph, &cfg, &imports,
            &graph_result, &import_result)) {
        check(0, "could not build trailing-default alias graph");
        return;
    }
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    result = lower_graph(&hir, &graph, graph_result.revision, &imports, &map);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-type-alias: defaults lowering: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    check(result.error_count == 0u,
        "trailing type defaults did not lower successfully");
    if (result.error_count != 0u) goto cleanup;

    a = find_item(&hir, "A");
    b = find_item(&hir, "B");
    forward = find_item(&hir, "Forward");
    nested = find_item(&hir, "Nested");
    marker = find_item(&hir, "Marker");
    defaults = find_item(&hir, "Defaults");
    check(a != NULL && b != NULL && forward != NULL && nested != NULL
        && marker != NULL && defaults != NULL
        && defaults->kind == CM_HIR_ITEM_STRUCT
        && defaults->data.aggregate_item.field_count == 12u,
        "defaulted alias declarations or uses are missing");
    if (a == NULL || b == NULL || forward == NULL || nested == NULL
        || marker == NULL || defaults == NULL
        || defaults->kind != CM_HIR_ITEM_STRUCT
        || defaults->data.aggregate_item.field_count != 12u) {
        goto cleanup;
    }

    parameter = find_generic_at(&hir, a->definition, 0u);
    check(parameter != NULL && parameter->has_default
        && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE
        && type_is_integer(&hir, parameter->default_argument.data.type,
            CM_HIR_INT_U8),
        "A did not retain its structural HIR type default");
    parameter = find_generic_at(&hir, b->definition, 1u);
    default_type = parameter == NULL ? NULL : cm_hir_get_type(&hir,
        parameter->default_argument.data.type);
    check(parameter != NULL && parameter->has_default
        && parameter->default_argument.kind == CM_HIR_GENERIC_ARG_TYPE
        && default_type != NULL
        && default_type->kind == CM_HIR_TYPE_PARAMETER_KIND
        && default_type->data.parameter_type.parameter
            == b->generic_parameter_start,
        "B<U = T> did not retain its earlier-parameter default");
    parameter = find_generic_at(&hir, forward->definition, 0u);
    check(parameter != NULL && parameter->has_default
        && type_is_adt(&hir, parameter->default_argument.data.type,
            marker->definition),
        "forward nominal default did not resolve to Marker");
    parameter = find_generic_at(&hir, nested->definition, 0u);
    check(parameter != NULL && parameter->has_default
        && type_is_integer(&hir, parameter->default_argument.data.type,
            CM_HIR_INT_U8),
        "default alias application did not normalize structurally");

    check(type_is_integer(&hir,
            defaults->data.aggregate_item.fields[0].type, CM_HIR_INT_U8),
        "omitted A default did not instantiate u8");
    check(type_is_integer(&hir,
            defaults->data.aggregate_item.fields[1].type, CM_HIR_INT_U16),
        "explicit A argument did not override its default");
    check(tuple_type_matches_integers(&hir,
            defaults->data.aggregate_item.fields[2].type,
            CM_HIR_INT_U32, CM_HIR_INT_U32),
        "B<u32> did not substitute the earlier T into U's default");
    check(tuple_type_matches_integers(&hir,
            defaults->data.aggregate_item.fields[3].type,
            CM_HIR_INT_U32, CM_HIR_INT_U16),
        "explicit B second argument did not override its default");
    check(tuple_type_matches_integers(&hir,
            defaults->data.aggregate_item.fields[4].type,
            CM_HIR_INT_U8, CM_HIR_INT_U8),
        "fully omitted sequential defaults did not instantiate in order");
    check(tuple_type_matches_integers(&hir,
            defaults->data.aggregate_item.fields[5].type,
            CM_HIR_INT_U16, CM_HIR_INT_U16),
        "partial sequential defaults did not use the explicit earlier value");
    check(type_is_adt(&hir,
            defaults->data.aggregate_item.fields[6].type,
            marker->definition),
        "omitted forward nominal default did not instantiate Marker");
    check(type_is_integer(&hir,
            defaults->data.aggregate_item.fields[7].type, CM_HIR_INT_U8),
        "omitted default through another alias did not instantiate u8");
    check(lifetime_default_type_matches(&hir,
            defaults->data.aggregate_item.fields[8].type,
            CM_HIR_REGION_EARLY_BOUND),
        "explicit lifetime plus omitted trailing type default was lost");
    check(lifetime_default_type_matches(&hir,
            defaults->data.aggregate_item.fields[9].type,
            CM_HIR_REGION_INFER),
        "omitted lifetime plus omitted trailing type default was lost");
    check(reference_type_matches_integer(&hir,
            defaults->data.aggregate_item.fields[10].type,
            CM_HIR_REGION_EARLY_BOUND, CM_HIR_INT_U8),
        "type default did not retain an explicit earlier lifetime");
    check(reference_type_matches_integer(&hir,
            defaults->data.aggregate_item.fields[11].type,
            CM_HIR_REGION_INFER, CM_HIR_INT_U8),
        "type default did not receive an inferred earlier lifetime");
    check(semantic_roots_are_alias_free(&hir),
        "defaulted aliases retained a transient semantic root");

cleanup:
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    destroy_graph(&sources, &graph, &imports);
}

static void test_imported_reexported_alias(void)
{
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmImportResolver imports;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerResult result;
    const CmHirItem *consumer;
    const CmHirItem *marker;

    if (!build_file_graph("tests/hir/fixtures/type-aliases/lib.rs",
            &sources, &graph, &cfg, &imports, &graph_result,
            &import_result)) {
        check(0, "could not build imported alias fixture");
        return;
    }
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    result = lower_graph(&hir, &graph, graph_result.revision, &imports, &map);
    if (result.error_count != 0u) {
        fprintf(stderr, "hir-type-alias: imported lowering: %s: %s\n",
            cm_hir_lower_error_kind_name(result.first_error.kind),
            result.first_error.message);
    }
    check(result.error_count == 0u,
        "imported/reexported alias did not lower successfully");
    if (result.error_count == 0u) {
        consumer = find_item(&hir, "Consumer");
        marker = find_item(&hir, "Marker");
        check(consumer != NULL && consumer->kind == CM_HIR_ITEM_STRUCT
            && consumer->data.aggregate_item.field_count == 2u
            && marker != NULL && marker->kind == CM_HIR_ITEM_STRUCT,
            "imported alias fixture items are missing");
        if (consumer != NULL && consumer->kind == CM_HIR_ITEM_STRUCT
            && consumer->data.aggregate_item.field_count == 2u
            && marker != NULL) {
            check(pair_type_matches_adt(&hir,
                    consumer->data.aggregate_item.fields[0].type,
                    marker->definition),
                "imported/reexported alias did not instantiate its default");
            check(pair_type_matches_integer(&hir,
                    consumer->data.aggregate_item.fields[1].type,
                    CM_HIR_INT_U16),
                "imported/reexported alias did not accept an override");
        }
        check(semantic_roots_are_alias_free(&hir),
            "imported alias graph retained a transient semantic root");
    }
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    destroy_graph(&sources, &graph, &imports);
}

static void test_generated_alias(void)
{
    static const char source[] =
        "mod defs {"
        " macro_rules! make { () => {"
        "  pub type Alias<T = u8> = (T, *const T);"
        " } }"
        " make!();"
        "}"
        "use crate::defs::Alias as Imported;"
        "struct Consumer { defaulted: Imported, overridden: Imported<u16> }";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmImportResolver imports;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerResult result;
    const CmHirItem *consumer;

    if (!build_memory_graph(source, &sources, &graph, &cfg, &imports,
            &graph_result, &import_result)) {
        check(0, "could not build generated alias graph");
        return;
    }
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    result = lower_graph(&hir, &graph, graph_result.revision, &imports, &map);
    check(result.error_count == 0u,
        "generated imported alias did not lower successfully");
    consumer = find_item(&hir, "Consumer");
    check(consumer != NULL && consumer->kind == CM_HIR_ITEM_STRUCT
        && consumer->data.aggregate_item.field_count == 2u
        && pair_type_matches_integer(&hir,
            consumer->data.aggregate_item.fields[0].type, CM_HIR_INT_U8)
        && pair_type_matches_integer(&hir,
            consumer->data.aggregate_item.fields[1].type, CM_HIR_INT_U16),
        "generated imported alias defaults or overrides were lost");
    check(result.error_count != 0u || semantic_roots_are_alias_free(&hir),
        "generated alias graph retained a transient semantic root");
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    destroy_graph(&sources, &graph, &imports);
}

typedef struct FailureCase {
    const char *source;
    CmHirLowerErrorKind expected_kind;
    const char *expected_message;
} FailureCase;

static void test_failures_are_transactional(void)
{
    static const FailureCase cases[] = {
        { "type Pair<T> = (T, T); struct Bad { value: Pair }",
            CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH, NULL },
        { "type Pair<T> = (T, T); struct Bad { value: Pair<u8, u16> }",
            CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH, NULL },
        { "type Pair<T> = (T, T); struct Bad { value: Pair<'static> }",
            CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH, NULL },
        { "type A = A; struct Bad { value: A }",
            CM_HIR_LOWER_ALIAS_CYCLE, NULL },
        { "type A = (A, A); struct Bad { value: A }",
            CM_HIR_LOWER_ALIAS_CYCLE, NULL },
        { "type A<T> = A<T>; struct Bad { value: A<u8> }",
            CM_HIR_LOWER_ALIAS_CYCLE, NULL },
        { "mod left { pub type A = crate::right::B; }"
          "mod right { pub type B = crate::left::A; }"
          "struct Bad { value: left::A }", CM_HIR_LOWER_ALIAS_CYCLE,
            NULL },
        { "type A<T = T> = T;", CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            "generic type default references itself or a later parameter" },
        { "type Good<T = u8> = T; type Bad<T = T> = T;",
            CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            "generic type default references itself or a later parameter" },
        { "type A<T = U, U = u8> = T;",
            CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            "generic type default references itself or a later parameter" },
        { "type A<T = u8, U> = (T, U);",
            CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            "a required generic parameter follows a defaulted one" },
        { "type A<T = u8, 'a> = T;",
            CM_HIR_LOWER_UNSUPPORTED_GENERIC,
            "lifetime parameters must precede type parameters" },
        { "type A<T = B> = T; type B = A; struct Good;",
            CM_HIR_LOWER_ALIAS_CYCLE, NULL },
        { "type A<T = u8> = T; struct Bad { value: A<u8, u16> }",
            CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH, NULL },
        { "type A<'a, T = u8> = T;"
          "struct Bad { value: A<'static, 'static> }",
            CM_HIR_LOWER_ALIAS_ARGUMENT_MISMATCH, NULL }
    };
    size_t index;

    for (index = 0u; index < CM_ARRAY_LEN(cases); ++index) {
        CmSourceSet sources;
        CmModuleGraph graph;
        CmCfgSet cfg;
        CmImportResolver imports;
        CmModuleGraphResult graph_result;
        CmImportResult import_result;
        CmHirContext hir;
        CmHirModuleMap map;
        CmHirLowerResult result;
        CmInternId sentinel_name;
        CmHirCrateId sentinel_crate;
        CmHirModuleId sentinel_root;
        CmSpan sentinel_span;

        if (!build_memory_graph(cases[index].source, &sources, &graph, &cfg,
                &imports, &graph_result, &import_result)) {
            check(0, "could not build rejected alias graph");
            continue;
        }
        cm_hir_context_init(&hir);
        cm_hir_module_map_init(&map);
        sentinel_name = cm_hir_intern(&hir, "alias-sentinel");
        sentinel_span.source = 993u;
        sentinel_span.start = 6u;
        sentinel_span.end = 7u;
        check(cm_hir_create_crate(&hir, sentinel_name,
            CM_HIR_EDITION_2024, sentinel_span, &sentinel_crate,
            &sentinel_root) == CM_HIR_OK,
            "could not create alias rollback sentinel");

        result = lower_graph(&hir, &graph, graph_result.revision, &imports,
            &map);
        if (!(result.error_count == 1u
                && result.crate_id == CM_HIR_CRATE_NONE
                && result.root_module == CM_HIR_MODULE_NONE
                && result.lowered_item_count == 0u
                && result.first_error.kind == cases[index].expected_kind
                && (cases[index].expected_message == NULL
                    || strcmp(result.first_error.message,
                        cases[index].expected_message) == 0)
                && hir.crates.len == 1u && hir.modules.len == 1u
                && hir.items.len == 0u && hir.types.len == 0u
                && hir.generic_parameters.len == 0u
                && hir.definitions.len == 1u
                && cm_interner_length(&hir.strings) == 1u
                && hir_name_is(&hir, sentinel_name, "alias-sentinel")
                && cm_hir_get_crate(&hir, sentinel_crate) != NULL
                && cm_hir_get_module(&hir, sentinel_root) != NULL
                && cm_hir_module_map_count(&map) == 0u)) {
            fprintf(stderr,
                "hir-type-alias: failure case %u: errors=%u kind=%s "
                "message=%s crates=%u modules=%u items=%u types=%u "
                "generics=%u defs=%u strings=%u map=%u\n",
                (unsigned int)index, (unsigned int)result.error_count,
                cm_hir_lower_error_kind_name(result.first_error.kind),
                result.first_error.message,
                (unsigned int)hir.crates.len, (unsigned int)hir.modules.len,
                (unsigned int)hir.items.len, (unsigned int)hir.types.len,
                (unsigned int)hir.generic_parameters.len,
                (unsigned int)hir.definitions.len,
                (unsigned int)cm_interner_length(&hir.strings),
                (unsigned int)cm_hir_module_map_count(&map));
        }
        check(result.error_count == 1u
            && result.crate_id == CM_HIR_CRATE_NONE
            && result.root_module == CM_HIR_MODULE_NONE
            && result.lowered_item_count == 0u
            && result.first_error.kind == cases[index].expected_kind
            && (cases[index].expected_message == NULL
                || strcmp(result.first_error.message,
                    cases[index].expected_message) == 0)
            && hir.crates.len == 1u && hir.modules.len == 1u
            && hir.items.len == 0u && hir.types.len == 0u
            && hir.generic_parameters.len == 0u
            && hir.definitions.len == 1u
            && cm_interner_length(&hir.strings) == 1u
            && hir_name_is(&hir, sentinel_name, "alias-sentinel")
            && cm_hir_get_crate(&hir, sentinel_crate) != NULL
            && cm_hir_get_module(&hir, sentinel_root) != NULL
            && cm_hir_module_map_count(&map) == 0u,
            "alias failure was misclassified or escaped transaction rewind");

        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        destroy_graph(&sources, &graph, &imports);
    }
}

static void init_projection_test_item(CmHirContext *hir, CmHirItem *item,
    CmHirItemKind kind, CmHirDefId definition, CmHirDefId parent,
    CmHirModuleId module, const char *name, CmSpan span)
{
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->definition = definition;
    item->owner_module = module;
    item->parent_definition = parent;
    item->name = cm_hir_intern(hir, name);
    item->visibility.kind = CM_HIR_VIS_PRIVATE;
    item->visibility.restriction = cm_hir_def_id_none();
    item->span = span;
    item->generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
}

static void test_projection_normalization(void)
{
    CmHirContext hir;
    CmSpan span;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId trait_definition;
    CmHirDefId associated_definition;
    CmHirDefId other_trait_definition;
    CmHirDefId id_definition;
    CmHirDefId project_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId trait_parameter_id;
    CmHirGenericParamId associated_parameter_id;
    CmHirGenericParamId id_parameter_id;
    CmHirGenericParamId project_t_id;
    CmHirGenericParamId project_u_id;
    CmHirGenericArg argument;
    CmHirGenericArg trait_arguments[1];
    CmHirGenericArg associated_arguments[1];
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId u8_type;
    CmHirTypeId u16_type;
    CmHirTypeId id_parameter_type;
    CmHirTypeId project_t_type;
    CmHirTypeId project_u_type;
    CmHirTypeId id_t_application;
    CmHirTypeId id_u_application;
    CmHirTypeId projection_type;
    CmHirTypeId project_application;
    CmHirTypeId normalized_projection;
    CmHirTypeId id_u16_application;
    CmHirTypeId failing_projection;
    CmHirTypeId ownership_projection;
    CmHirTypeAliasResult result;
    const CmHirType *normalized;
    const CmHirType *original;
    CmHirType *malformed;
    size_t type_count;
    size_t arena_bytes;

    cm_hir_context_init(&hir);
    span.source = 91u;
    span.start = 3u;
    span.end = 9u;
    check(cm_hir_create_crate(&hir, cm_hir_intern(&hir, "projection_api"),
            CM_HIR_EDITION_2024, span, &crate_id, &root_module)
            == CM_HIR_OK,
        "projection setup could not create a crate");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = span;
    type.data.integer_type.kind = CM_HIR_INT_U8;
    check(cm_hir_add_type(&hir, &type, &u8_type) == CM_HIR_OK,
        "projection setup could not add u8");
    type.data.integer_type.kind = CM_HIR_INT_U16;
    check(cm_hir_add_type(&hir, &type, &u16_type) == CM_HIR_OK,
        "projection setup could not add u16");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &trait_definition) == CM_HIR_OK,
        "projection setup could not reserve its trait");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = trait_definition;
    parameter.name = cm_hir_intern(&hir, "R");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter, &trait_parameter_id)
            == CM_HIR_OK,
        "projection setup could not add its trait parameter");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TRAIT,
        trait_definition, cm_hir_def_id_none(), root_module, "ProjectTrait",
        span);
    item.generic_parameter_start = trait_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "projection setup could not bind its trait");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &associated_definition) == CM_HIR_OK,
        "projection setup could not reserve its associated alias");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = associated_definition;
    parameter.name = cm_hir_intern(&hir, "A");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter,
            &associated_parameter_id) == CM_HIR_OK,
        "projection setup could not add its associated parameter");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TYPE_ALIAS,
        associated_definition, trait_definition, root_module, "Assoc", span);
    item.generic_parameter_start = associated_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "projection setup could not bind its associated alias");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &other_trait_definition) == CM_HIR_OK,
        "projection setup could not reserve its other trait");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TRAIT,
        other_trait_definition, cm_hir_def_id_none(), root_module,
        "OtherTrait", span);
    item.data.trait_item.safety = CM_HIR_SAFE;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "projection setup could not bind its other trait");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &id_definition) == CM_HIR_OK,
        "projection setup could not reserve Id");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = id_definition;
    parameter.name = cm_hir_intern(&hir, "X");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter, &id_parameter_id)
            == CM_HIR_OK,
        "projection setup could not add Id's parameter");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = span;
    type.data.parameter_type.parameter = id_parameter_id;
    check(cm_hir_add_type(&hir, &type, &id_parameter_type) == CM_HIR_OK,
        "projection setup could not add Id's parameter type");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TYPE_ALIAS,
        id_definition, cm_hir_def_id_none(), root_module, "Id", span);
    item.generic_parameter_start = id_parameter_id;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = id_parameter_type;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "projection setup could not bind Id");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &project_definition) == CM_HIR_OK,
        "projection setup could not reserve Project");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = project_definition;
    parameter.name = cm_hir_intern(&hir, "T");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter, &project_t_id)
            == CM_HIR_OK,
        "projection setup could not add Project's T");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = span;
    type.data.parameter_type.parameter = project_t_id;
    check(cm_hir_add_type(&hir, &type, &project_t_type) == CM_HIR_OK,
        "projection setup could not add Project's T type");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = project_definition;
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&hir, "U");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter, &project_u_id)
            == CM_HIR_OK,
        "projection setup could not add Project's U");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = span;
    type.data.parameter_type.parameter = project_u_id;
    check(cm_hir_add_type(&hir, &type, &project_u_type) == CM_HIR_OK,
        "projection setup could not add Project's U type");
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = project_t_type;
    check(cm_hir_set_generic_param_default(&hir, project_u_id, &argument)
            == CM_HIR_OK,
        "projection setup could not default U to T");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span = span;
    type.data.named_type.definition = id_definition;
    type.data.named_type.arguments = &argument;
    type.data.named_type.argument_count = 1u;
    check(cm_hir_add_type(&hir, &type, &id_t_application) == CM_HIR_OK,
        "projection setup could not add Id<T>");
    argument.data.type = project_u_type;
    check(cm_hir_add_type(&hir, &type, &id_u_application) == CM_HIR_OK,
        "projection setup could not add Id<U>");

    memset(trait_arguments, 0, sizeof(trait_arguments));
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_arguments[0].data.type = id_u_application;
    memset(associated_arguments, 0, sizeof(associated_arguments));
    associated_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    associated_arguments[0].data.type = id_t_application;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = span;
    type.data.projection_type.self_type = id_t_application;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.trait_type.arguments = trait_arguments;
    type.data.projection_type.trait_type.argument_count = 1u;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    type.data.projection_type.associated_type.arguments =
        associated_arguments;
    type.data.projection_type.associated_type.argument_count = 1u;
    check(cm_hir_add_type(&hir, &type, &projection_type) == CM_HIR_OK,
        "projection setup could not add its nested projection");

    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TYPE_ALIAS,
        project_definition, cm_hir_def_id_none(), root_module, "Project",
        span);
    item.generic_parameter_start = project_t_id;
    item.generic_parameter_count = 2u;
    item.data.type_alias_item.target = projection_type;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "projection setup could not bind Project");
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u8_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span.source = 91u;
    type.span.start = 40u;
    type.span.end = 47u;
    type.data.named_type.definition = project_definition;
    type.data.named_type.arguments = &argument;
    type.data.named_type.argument_count = 1u;
    check(cm_hir_add_type(&hir, &type, &project_application) == CM_HIR_OK,
        "projection setup could not add Project<u8>");

    result = cm_hir_normalize_type_aliases(&hir, project_application);
    normalized = cm_hir_get_type(&hir, result.type);
    original = cm_hir_get_type(&hir, projection_type);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type != CM_HIR_TYPE_NONE
        && result.allocated_type_count != 0u
        && normalized != NULL
        && normalized->kind == CM_HIR_TYPE_PROJECTION_KIND
        && normalized->span.start == 40u && normalized->span.end == 47u
        && type_is_integer(&hir,
            normalized->data.projection_type.self_type, CM_HIR_INT_U8)
        && cm_hir_def_id_equal(
            normalized->data.projection_type.trait_type.definition,
            trait_definition)
        && normalized->data.projection_type.trait_type.argument_count == 1u
        && normalized->data.projection_type.trait_type.arguments != NULL
        && normalized->data.projection_type.trait_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && type_is_integer(&hir,
            normalized->data.projection_type.trait_type.arguments[0]
                .data.type,
            CM_HIR_INT_U8)
        && cm_hir_def_id_equal(
            normalized->data.projection_type.associated_type.definition,
            associated_definition)
        && normalized->data.projection_type.associated_type.argument_count
            == 1u
        && normalized->data.projection_type.associated_type.arguments != NULL
        && normalized->data.projection_type.associated_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE
        && type_is_integer(&hir,
            normalized->data.projection_type.associated_type.arguments[0]
                .data.type,
            CM_HIR_INT_U8)
        && type_root_is_alias_free(&hir, result.type, 0u),
        "projection normalization did not substitute "
        "self/trait/associated arguments");
    check(original != NULL && original->kind == CM_HIR_TYPE_PROJECTION_KIND
        && original->data.projection_type.self_type == id_t_application,
        "projection normalization modified its source node");

    normalized_projection = result.type;
    type_count = hir.types.len;
    arena_bytes = cm_arena_bytes_used(&hir.storage);
    result = cm_hir_normalize_type_aliases(&hir, normalized_projection);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type == normalized_projection
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes,
        "unchanged projection normalization grew the type arena");

    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u16_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span = span;
    type.data.named_type.definition = id_definition;
    type.data.named_type.arguments = &argument;
    type.data.named_type.argument_count = 1u;
    check(cm_hir_add_type(&hir, &type, &id_u16_application) == CM_HIR_OK,
        "projection setup could not add Id<u16>");
    memset(trait_arguments, 0, sizeof(trait_arguments));
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_arguments[0].data.type = u8_type;
    memset(associated_arguments, 0, sizeof(associated_arguments));
    associated_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    associated_arguments[0].data.type = u8_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = span;
    type.data.projection_type.self_type = id_u16_application;
    type.data.projection_type.trait_type.definition = trait_definition;
    type.data.projection_type.trait_type.arguments = trait_arguments;
    type.data.projection_type.trait_type.argument_count = 1u;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    type.data.projection_type.associated_type.arguments =
        associated_arguments;
    type.data.projection_type.associated_type.argument_count = 1u;
    check(cm_hir_add_type(&hir, &type, &failing_projection) == CM_HIR_OK,
        "projection setup could not add its failing projection");
    malformed = (CmHirType *)cm_vec_at(&hir.types,
        (size_t)failing_projection - 1u);
    check(malformed != NULL,
        "projection setup could not borrow its failing projection");
    if (malformed != NULL) {
        malformed->data.projection_type.associated_type.arguments[0].kind =
            CM_HIR_GENERIC_ARG_CONST;
        malformed->data.projection_type.associated_type.arguments[0]
            .data.constant.kind = CM_HIR_CONST_VALUE;
        malformed->data.projection_type.associated_type.arguments[0]
            .data.constant.type = u8_type;
    }
    type_count = hir.types.len;
    arena_bytes = cm_arena_bytes_used(&hir.storage);
    result = cm_hir_normalize_type_aliases(&hir, failing_projection);
    check(result.status == CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST
        && result.type == CM_HIR_TYPE_NONE
        && result.source_type == failing_projection
        && cm_hir_def_id_equal(result.alias_definition,
            associated_definition)
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes,
        "projection failure did not rewind nested normalization");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PROJECTION_KIND;
    type.span = span;
    type.data.projection_type.self_type = u8_type;
    type.data.projection_type.trait_type.definition = trait_definition;
    trait_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    trait_arguments[0].data.type = u8_type;
    type.data.projection_type.trait_type.arguments = trait_arguments;
    type.data.projection_type.trait_type.argument_count = 1u;
    type.data.projection_type.associated_type.definition =
        associated_definition;
    associated_arguments[0].kind = CM_HIR_GENERIC_ARG_TYPE;
    associated_arguments[0].data.type = u8_type;
    type.data.projection_type.associated_type.arguments =
        associated_arguments;
    type.data.projection_type.associated_type.argument_count = 1u;
    check(cm_hir_add_type(&hir, &type, &ownership_projection) == CM_HIR_OK,
        "projection setup could not add its ownership projection");
    malformed = (CmHirType *)cm_vec_at(&hir.types,
        (size_t)ownership_projection - 1u);
    check(malformed != NULL,
        "projection setup could not borrow its ownership projection");
    if (malformed != NULL) {
        malformed->data.projection_type.trait_type.definition =
            other_trait_definition;
    }
    type_count = hir.types.len;
    arena_bytes = cm_arena_bytes_used(&hir.storage);
    result = cm_hir_normalize_type_aliases(&hir, ownership_projection);
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_TYPE
        && result.type == CM_HIR_TYPE_NONE
        && result.source_type == ownership_projection
        && cm_hir_def_id_equal(result.alias_definition,
            associated_definition)
        && result.hir_status == CM_HIR_INVARIANT_VIOLATION
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes,
        "projection normalizer accepted an associated alias from another "
        "trait");

    cm_hir_context_destroy(&hir);
}

static void test_normalizer_api_transaction(void)
{
    CmHirContext hir;
    CmSpan span;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId alias_definition;
    CmHirDefId default_definition;
    CmHirDefId foreign_definition;
    CmHirDefId malformed_definition;
    CmHirDefId malformed_lifetime_definition;
    CmHirItem alias_item;
    CmHirItemId alias_item_id;
    CmHirGenericParam foreign_parameter;
    CmHirGenericParam default_parameter;
    CmHirGenericArg default_argument;
    CmHirGenericParamId default_parameter_id;
    CmHirGenericParamId foreign_parameter_id;
    CmHirGenericParamId foreign_lifetime_id;
    CmHirType type;
    CmHirTypeId u8_type;
    CmHirTypeId application_type;
    CmHirTypeId default_parameter_type;
    CmHirTypeId default_application_type;
    CmHirTypeId foreign_parameter_type;
    CmHirTypeId malformed_application_type;
    CmHirTypeId foreign_lifetime_type;
    CmHirTypeId malformed_lifetime_application_type;
    CmHirTypeId error_type;
    CmHirTypeId tuple_type;
    CmHirTypeId deep_type;
    CmHirTypeId tuple_elements[2];
    CmHirTypeAliasResult result;
    const CmHirType *normalized;
    size_t type_count;
    uint32_t depth_index;

    cm_hir_context_init(&hir);
    span.source = 77u;
    span.start = 10u;
    span.end = 20u;
    check(cm_hir_create_crate(&hir, cm_hir_intern(&hir, "alias_api"),
            CM_HIR_EDITION_2024, span, &crate_id, &root_module)
            == CM_HIR_OK,
        "normalizer API setup could not create a crate");
    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &alias_definition) == CM_HIR_OK,
        "normalizer API setup could not reserve an alias definition");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = span;
    type.data.integer_type.kind = CM_HIR_INT_U8;
    check(cm_hir_add_type(&hir, &type, &u8_type) == CM_HIR_OK,
        "normalizer API setup could not add u8");

    memset(&alias_item, 0, sizeof(alias_item));
    alias_item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    alias_item.definition = alias_definition;
    alias_item.owner_module = root_module;
    alias_item.parent_definition = cm_hir_def_id_none();
    alias_item.name = cm_hir_intern(&hir, "Alias");
    alias_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    alias_item.visibility.restriction = cm_hir_def_id_none();
    alias_item.span = span;
    alias_item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    alias_item.data.type_alias_item.target = u8_type;
    check(cm_hir_add_item(&hir, &alias_item, &alias_item_id) == CM_HIR_OK,
        "normalizer API setup could not bind the alias item");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span.source = 77u;
    type.span.start = 30u;
    type.span.end = 35u;
    type.data.named_type.definition = alias_definition;
    check(cm_hir_add_type(&hir, &type, &application_type) == CM_HIR_OK,
        "normalizer API setup could not add an alias application");

    type_count = hir.types.len;
    result = cm_hir_normalize_type_aliases(&hir, application_type);
    normalized = cm_hir_get_type(&hir, result.type);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.allocated_type_count == 1u
        && hir.types.len == type_count + 1u
        && normalized != NULL
        && normalized->kind == CM_HIR_TYPE_INTEGER_KIND
        && normalized->data.integer_type.kind == CM_HIR_INT_U8
        && normalized->span.source == 77u
        && normalized->span.start == 30u
        && normalized->span.end == 35u,
        "normalizer API success lost allocation or use-site span metadata");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &default_definition) == CM_HIR_OK,
        "normalizer API setup could not reserve a defaulted alias");
    memset(&default_parameter, 0, sizeof(default_parameter));
    default_parameter.kind = CM_HIR_GENERIC_TYPE;
    default_parameter.owner = default_definition;
    default_parameter.name = cm_hir_intern(&hir, "T");
    default_parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &default_parameter,
            &default_parameter_id) == CM_HIR_OK,
        "normalizer API setup could not add a defaulted parameter");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = span;
    type.data.parameter_type.parameter = default_parameter_id;
    check(cm_hir_add_type(&hir, &type, &default_parameter_type)
            == CM_HIR_OK,
        "normalizer API setup could not add its parameter type");
    memset(&default_argument, 0, sizeof(default_argument));
    default_argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    default_argument.data.type = u8_type;
    check(cm_hir_set_generic_param_default(&hir, default_parameter_id,
            &default_argument) == CM_HIR_OK,
        "normalizer API setup could not assign its type default");
    memset(&alias_item, 0, sizeof(alias_item));
    alias_item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    alias_item.definition = default_definition;
    alias_item.owner_module = root_module;
    alias_item.parent_definition = cm_hir_def_id_none();
    alias_item.name = cm_hir_intern(&hir, "Defaulted");
    alias_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    alias_item.visibility.restriction = cm_hir_def_id_none();
    alias_item.span = span;
    alias_item.generic_parameter_start = default_parameter_id;
    alias_item.generic_parameter_count = 1u;
    alias_item.data.type_alias_item.target = default_parameter_type;
    check(cm_hir_add_item(&hir, &alias_item, &alias_item_id) == CM_HIR_OK,
        "normalizer API setup could not bind its defaulted alias");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span = span;
    type.data.named_type.definition = default_definition;
    check(cm_hir_add_type(&hir, &type, &default_application_type)
            == CM_HIR_OK,
        "normalizer API setup could not add an omitted-default use");
    type_count = hir.types.len;
    result = cm_hir_normalize_type_aliases(&hir,
        default_application_type);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.allocated_type_count == 1u
        && hir.types.len == type_count + 1u
        && type_is_integer(&hir, result.type, CM_HIR_INT_U8),
        "public normalizer did not instantiate a missing trailing default");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &foreign_definition) == CM_HIR_OK,
        "normalizer API setup could not reserve a foreign owner");
    memset(&foreign_parameter, 0, sizeof(foreign_parameter));
    foreign_parameter.kind = CM_HIR_GENERIC_LIFETIME;
    foreign_parameter.owner = foreign_definition;
    foreign_parameter.name = cm_hir_intern(&hir, "'a");
    foreign_parameter.span = span;
    foreign_parameter.declared_type = CM_HIR_TYPE_NONE;
    check(cm_hir_add_generic_param(&hir, &foreign_parameter,
            &foreign_lifetime_id) == CM_HIR_OK,
        "normalizer API setup could not add a foreign lifetime");
    memset(&foreign_parameter, 0, sizeof(foreign_parameter));
    foreign_parameter.kind = CM_HIR_GENERIC_TYPE;
    foreign_parameter.owner = foreign_definition;
    foreign_parameter.index = 1u;
    foreign_parameter.name = cm_hir_intern(&hir, "T");
    foreign_parameter.span = span;
    foreign_parameter.declared_type = CM_HIR_TYPE_NONE;
    check(cm_hir_add_generic_param(&hir, &foreign_parameter,
            &foreign_parameter_id) == CM_HIR_OK,
        "normalizer API setup could not add a foreign generic");
    memset(&alias_item, 0, sizeof(alias_item));
    alias_item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    alias_item.definition = foreign_definition;
    alias_item.owner_module = root_module;
    alias_item.parent_definition = cm_hir_def_id_none();
    alias_item.name = cm_hir_intern(&hir, "Foreign");
    alias_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    alias_item.visibility.restriction = cm_hir_def_id_none();
    alias_item.span = span;
    alias_item.generic_parameter_start = foreign_lifetime_id;
    alias_item.generic_parameter_count = 2u;
    alias_item.data.type_alias_item.target = u8_type;
    check(cm_hir_add_item(&hir, &alias_item, &alias_item_id) == CM_HIR_OK,
        "normalizer API setup could not bind a foreign generic owner");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &malformed_definition) == CM_HIR_OK,
        "normalizer API setup could not reserve a malformed alias");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = span;
    type.data.parameter_type.parameter = foreign_parameter_id;
    check(cm_hir_add_type(&hir, &type, &foreign_parameter_type) == CM_HIR_OK,
        "normalizer API setup could not add a foreign parameter type");
    memset(&alias_item, 0, sizeof(alias_item));
    alias_item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    alias_item.definition = malformed_definition;
    alias_item.owner_module = root_module;
    alias_item.parent_definition = cm_hir_def_id_none();
    alias_item.name = cm_hir_intern(&hir, "Malformed");
    alias_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    alias_item.visibility.restriction = cm_hir_def_id_none();
    alias_item.span = span;
    alias_item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    alias_item.data.type_alias_item.target = foreign_parameter_type;
    check(cm_hir_add_item(&hir, &alias_item, &alias_item_id) == CM_HIR_OK,
        "normalizer API setup could not bind a malformed alias");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span = span;
    type.data.named_type.definition = malformed_definition;
    check(cm_hir_add_type(&hir, &type, &malformed_application_type)
            == CM_HIR_OK,
        "normalizer API setup could not add a malformed application");
    type_count = hir.types.len;
    result = cm_hir_normalize_type_aliases(&hir,
        malformed_application_type);
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_ALIAS
        && result.type == CM_HIR_TYPE_NONE
        && result.source_type == foreign_parameter_type
        && cm_hir_def_id_equal(result.alias_definition,
            malformed_definition)
        && result.parameter == foreign_parameter_id
        && result.hir_status == CM_HIR_INVARIANT_VIOLATION
        && result.allocated_type_count == 0u
        && hir.types.len == type_count,
        "normalizer API accepted a parameter owned by another definition");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &malformed_lifetime_definition) == CM_HIR_OK,
        "normalizer API setup could not reserve a malformed lifetime alias");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_REFERENCE_KIND;
    type.span = span;
    type.data.reference_type.region.kind = CM_HIR_REGION_EARLY_BOUND;
    type.data.reference_type.region.data.parameter = foreign_lifetime_id;
    type.data.reference_type.pointee = u8_type;
    type.data.reference_type.mutability = CM_HIR_IMMUTABLE;
    check(cm_hir_add_type(&hir, &type, &foreign_lifetime_type) == CM_HIR_OK,
        "normalizer API setup could not add a foreign lifetime type");
    memset(&alias_item, 0, sizeof(alias_item));
    alias_item.kind = CM_HIR_ITEM_TYPE_ALIAS;
    alias_item.definition = malformed_lifetime_definition;
    alias_item.owner_module = root_module;
    alias_item.parent_definition = cm_hir_def_id_none();
    alias_item.name = cm_hir_intern(&hir, "MalformedLifetime");
    alias_item.visibility.kind = CM_HIR_VIS_PRIVATE;
    alias_item.visibility.restriction = cm_hir_def_id_none();
    alias_item.span = span;
    alias_item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
    alias_item.data.type_alias_item.target = foreign_lifetime_type;
    check(cm_hir_add_item(&hir, &alias_item, &alias_item_id) == CM_HIR_OK,
        "normalizer API setup could not bind a malformed lifetime alias");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span = span;
    type.data.named_type.definition = malformed_lifetime_definition;
    check(cm_hir_add_type(&hir, &type,
            &malformed_lifetime_application_type) == CM_HIR_OK,
        "normalizer API setup could not add a malformed lifetime use");
    type_count = hir.types.len;
    result = cm_hir_normalize_type_aliases(&hir,
        malformed_lifetime_application_type);
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_ALIAS
        && result.type == CM_HIR_TYPE_NONE
        && result.source_type == foreign_lifetime_type
        && cm_hir_def_id_equal(result.alias_definition,
            malformed_lifetime_definition)
        && result.parameter == foreign_lifetime_id
        && result.hir_status == CM_HIR_INVARIANT_VIOLATION
        && result.allocated_type_count == 0u
        && hir.types.len == type_count,
        "normalizer API accepted a lifetime owned by another definition");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ERROR_KIND;
    type.span = span;
    type.data.error_type.reason = cm_hir_intern(&hir, "malformed");
    check(cm_hir_add_type(&hir, &type, &error_type) == CM_HIR_OK,
        "normalizer API setup could not add a rejected error type");
    tuple_elements[0] = application_type;
    tuple_elements[1] = error_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = span;
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 2u;
    check(cm_hir_add_type(&hir, &type, &tuple_type) == CM_HIR_OK,
        "normalizer API setup could not add a failing tuple root");

    type_count = hir.types.len;
    result = cm_hir_normalize_type_aliases(&hir, tuple_type);
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_TYPE
        && result.type == CM_HIR_TYPE_NONE
        && result.source_type == error_type
        && result.hir_status == CM_HIR_INVALID_ID
        && result.allocated_type_count == 0u
        && hir.types.len == type_count,
        "normalizer API failure did not rewind its append-only allocation");

    result = cm_hir_normalize_type_aliases(NULL, application_type);
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_ARGUMENT
        && result.hir_status == CM_HIR_INVALID_ARGUMENT,
        "normalizer API did not classify a null context");

    deep_type = u8_type;
    for (depth_index = 0u; depth_index < 513u; ++depth_index) {
        CmHirTypeId next_type;

        memset(&type, 0, sizeof(type));
        type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
        type.span = span;
        type.data.raw_pointer_type.pointee = deep_type;
        type.data.raw_pointer_type.mutability = CM_HIR_IMMUTABLE;
        check(cm_hir_add_type(&hir, &type, &next_type) == CM_HIR_OK,
            "normalizer API setup could not build a deep type");
        deep_type = next_type;
    }
    type_count = hir.types.len;
    result = cm_hir_normalize_type_aliases(&hir, deep_type);
    check(result.status == CM_HIR_TYPE_ALIAS_RECURSION_LIMIT
        && result.type == CM_HIR_TYPE_NONE
        && result.source_type != CM_HIR_TYPE_NONE
        && result.allocated_type_count == 0u
        && hir.types.len == type_count,
        "normalizer API did not hard-error excessive type recursion");

    result = cm_hir_normalize_type_aliases(&hir,
        (CmHirTypeId)(hir.types.len + 100u));
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_TYPE
        && result.hir_status == CM_HIR_INVALID_ID,
        "normalizer API did not classify an invalid root ID");

    cm_hir_context_destroy(&hir);
}

static void test_type_instantiation_api(void)
{
    CmHirContext hir;
    CmSpan span;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId owner_definition;
    CmHirDefId alias_definition;
    CmHirDefId unbound_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId u8_type;
    CmHirTypeId u16_type;
    CmHirTypeId parameter_type;
    CmHirTypeId pointer_type;
    CmHirTypeId alias_application;
    CmHirTypeId error_type;
    CmHirTypeId failing_tuple;
    CmHirTypeId tuple_elements[2];
    CmHirGenericArg argument;
    CmHirTypeAliasResult result;
    const CmHirType *instantiated;
    size_t type_count;
    size_t arena_bytes;

    cm_hir_context_init(&hir);
    span.source = 88u;
    span.start = 4u;
    span.end = 12u;
    check(cm_hir_create_crate(&hir,
            cm_hir_intern(&hir, "instantiate_api"), CM_HIR_EDITION_2024,
            span, &crate_id, &root_module) == CM_HIR_OK,
        "instantiation setup could not create a crate");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = span;
    type.data.integer_type.kind = CM_HIR_INT_U8;
    check(cm_hir_add_type(&hir, &type, &u8_type) == CM_HIR_OK,
        "instantiation setup could not add u8");
    type.data.integer_type.kind = CM_HIR_INT_U16;
    check(cm_hir_add_type(&hir, &type, &u16_type) == CM_HIR_OK,
        "instantiation setup could not add u16");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &owner_definition) == CM_HIR_OK,
        "instantiation setup could not reserve its owner");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = owner_definition;
    parameter.name = cm_hir_intern(&hir, "T");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter, &parameter_id)
            == CM_HIR_OK,
        "instantiation setup could not add its owner parameter");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = span;
    type.data.parameter_type.parameter = parameter_id;
    check(cm_hir_add_type(&hir, &type, &parameter_type) == CM_HIR_OK,
        "instantiation setup could not add its parameter type");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TYPE_ALIAS,
        owner_definition, cm_hir_def_id_none(), root_module, "Template",
        span);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = parameter_type;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "instantiation setup could not bind its owner");

    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u8_type;
    type_count = hir.types.len;
    arena_bytes = cm_arena_bytes_used(&hir.storage);
    result = cm_hir_instantiate_type(&hir, parameter_type,
        owner_definition, &argument, 1u);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type == u8_type
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes,
        "direct parameter instantiation was not allocation-free");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
    type.span = span;
    type.data.raw_pointer_type.pointee = parameter_type;
    type.data.raw_pointer_type.mutability = CM_HIR_IMMUTABLE;
    check(cm_hir_add_type(&hir, &type, &pointer_type) == CM_HIR_OK,
        "instantiation setup could not add its structural root");
    argument.data.type = u16_type;
    type_count = hir.types.len;
    result = cm_hir_instantiate_type(&hir, pointer_type,
        owner_definition, &argument, 1u);
    instantiated = cm_hir_get_type(&hir, result.type);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.allocated_type_count == 1u
        && hir.types.len == type_count + 1u
        && instantiated != NULL
        && instantiated->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && instantiated->data.raw_pointer_type.pointee == u16_type,
        "structural instantiation did not substitute through a pointer");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &alias_definition) == CM_HIR_OK,
        "instantiation setup could not reserve an argument alias");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TYPE_ALIAS,
        alias_definition, cm_hir_def_id_none(), root_module, "U8Alias",
        span);
    item.data.type_alias_item.target = u8_type;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "instantiation setup could not bind an argument alias");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span = span;
    type.data.named_type.definition = alias_definition;
    check(cm_hir_add_type(&hir, &type, &alias_application) == CM_HIR_OK,
        "instantiation setup could not add an unnormalized argument");
    argument.data.type = alias_application;
    type_count = hir.types.len;
    arena_bytes = cm_arena_bytes_used(&hir.storage);
    result = cm_hir_instantiate_type(&hir, parameter_type,
        owner_definition, &argument, 1u);
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_TYPE
        && result.type == CM_HIR_TYPE_NONE
        && result.source_type == alias_application
        && cm_hir_def_id_equal(result.alias_definition, owner_definition)
        && result.parameter == parameter_id
        && result.hir_status == CM_HIR_INVARIANT_VIOLATION
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes,
        "instantiation accepted or retained an unnormalized argument");

    result = cm_hir_instantiate_type(&hir, parameter_type,
        owner_definition, NULL, 0u);
    check(result.status == CM_HIR_TYPE_ALIAS_ARGUMENT_COUNT
        && cm_hir_def_id_equal(result.alias_definition, owner_definition),
        "instantiation did not reject the wrong argument count");
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_LIFETIME;
    argument.data.lifetime.kind = CM_HIR_REGION_STATIC;
    result = cm_hir_instantiate_type(&hir, parameter_type,
        owner_definition, &argument, 1u);
    check(result.status == CM_HIR_TYPE_ALIAS_ARGUMENT_KIND
        && result.parameter == parameter_id,
        "instantiation did not reject the wrong argument kind");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &unbound_definition) == CM_HIR_OK,
        "instantiation setup could not reserve an unbound owner");
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u8_type;
    result = cm_hir_instantiate_type(&hir, parameter_type,
        unbound_definition, &argument, 1u);
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_ALIAS
        && cm_hir_def_id_equal(result.alias_definition,
            unbound_definition)
        && result.hir_status == CM_HIR_INVALID_ID,
        "instantiation accepted an unbound owner definition");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ERROR_KIND;
    type.span = span;
    type.data.error_type.reason = cm_hir_intern(&hir, "instantiate-error");
    check(cm_hir_add_type(&hir, &type, &error_type) == CM_HIR_OK,
        "instantiation setup could not add a rejected error type");
    tuple_elements[0] = pointer_type;
    tuple_elements[1] = error_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = span;
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 2u;
    check(cm_hir_add_type(&hir, &type, &failing_tuple) == CM_HIR_OK,
        "instantiation setup could not add its failing structural root");
    type_count = hir.types.len;
    arena_bytes = cm_arena_bytes_used(&hir.storage);
    result = cm_hir_instantiate_type(&hir, failing_tuple,
        owner_definition, &argument, 1u);
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_TYPE
        && result.type == CM_HIR_TYPE_NONE
        && result.source_type == error_type
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes,
        "failed structural instantiation did not rewind partial additions");

    result = cm_hir_instantiate_type(NULL, parameter_type,
        owner_definition, &argument, 1u);
    check(result.status == CM_HIR_TYPE_ALIAS_INVALID_ARGUMENT
        && result.hir_status == CM_HIR_INVALID_ARGUMENT,
        "instantiation did not classify a null context");

    cm_hir_context_destroy(&hir);
}

static void test_symbolic_self_normalization(void)
{
    CmHirContext hir;
    CmSpan span;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId trait_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId parameter_id;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId u8_type;
    CmHirTypeId parameter_type;
    CmHirTypeId self_type;
    CmHirTypeId tuple_type;
    CmHirTypeId tuple_elements[2];
    CmHirGenericArg argument;
    CmHirTypeAliasResult result;
    const CmHirType *normalized;
    const CmHirType *preserved_self;
    size_t type_count;

    cm_hir_context_init(&hir);
    span.source = 89u;
    span.start = 2u;
    span.end = 10u;
    check(cm_hir_create_crate(&hir,
            cm_hir_intern(&hir, "symbolic_self"), CM_HIR_EDITION_2024,
            span, &crate_id, &root_module) == CM_HIR_OK,
        "symbolic Self setup could not create a crate");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = span;
    type.data.integer_type.kind = CM_HIR_INT_U8;
    check(cm_hir_add_type(&hir, &type, &u8_type) == CM_HIR_OK,
        "symbolic Self setup could not add u8");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &trait_definition) == CM_HIR_OK,
        "symbolic Self setup could not reserve its trait");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = trait_definition;
    parameter.name = cm_hir_intern(&hir, "T");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter, &parameter_id)
            == CM_HIR_OK,
        "symbolic Self setup could not add its trait parameter");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = span;
    type.data.parameter_type.parameter = parameter_id;
    check(cm_hir_add_type(&hir, &type, &parameter_type) == CM_HIR_OK,
        "symbolic Self setup could not add its parameter type");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TRAIT,
        trait_definition, cm_hir_def_id_none(), root_module,
        "Symbolic", span);
    item.generic_parameter_start = parameter_id;
    item.generic_parameter_count = 1u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "symbolic Self setup could not bind its trait");

    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_SELF_KIND;
    type.span = span;
    type.data.self_type.owner = trait_definition;
    check(cm_hir_add_type(&hir, &type, &self_type) == CM_HIR_OK,
        "symbolic Self setup could not add Self");
    tuple_elements[0] = self_type;
    tuple_elements[1] = parameter_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_TUPLE_KIND;
    type.span = span;
    type.data.tuple_type.elements = tuple_elements;
    type.data.tuple_type.element_count = 2u;
    check(cm_hir_add_type(&hir, &type, &tuple_type) == CM_HIR_OK,
        "symbolic Self setup could not add its structural type");

    type_count = hir.types.len;
    result = cm_hir_normalize_type_aliases(&hir, tuple_type);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type == tuple_type
        && result.allocated_type_count == 0u
        && hir.types.len == type_count,
        "alias normalization did not preserve an unchanged symbolic Self");

    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = u8_type;
    result = cm_hir_instantiate_type(&hir, tuple_type,
        trait_definition, &argument, 1u);
    normalized = cm_hir_get_type(&hir, result.type);
    preserved_self = normalized == NULL
        || normalized->kind != CM_HIR_TYPE_TUPLE_KIND
        || normalized->data.tuple_type.element_count != 2u
        ? NULL
        : cm_hir_get_type(&hir,
            normalized->data.tuple_type.elements[0]);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type != CM_HIR_TYPE_NONE
        && result.type != tuple_type
        && result.allocated_type_count == 1u
        && normalized != NULL
        && normalized->kind == CM_HIR_TYPE_TUPLE_KIND
        && normalized->data.tuple_type.element_count == 2u
        && normalized->data.tuple_type.elements[0] == self_type
        && normalized->data.tuple_type.elements[1] == u8_type
        && preserved_self != NULL
        && preserved_self->kind == CM_HIR_TYPE_SELF_KIND
        && cm_hir_def_id_equal(preserved_self->data.self_type.owner,
            trait_definition),
        "type substitution replaced or erased symbolic Self");

    cm_hir_context_destroy(&hir);
}

static void test_nominal_const_parameter_normalization(void)
{
    static const char source[] =
        "struct Array<const N: usize>; "
        "trait Owner<const M: usize> { fn make() -> Array<M>; }";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmImportResolver imports;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerResult lower_result;
    const CmHirItem *array;
    const CmHirItem *owner;
    const CmHirItem *make;
    const CmHirGenericParam *array_parameter;
    const CmHirGenericParam *owner_parameter;
    CmHirGenericParamId owner_parameter_id;
    CmHirTypeId return_type_id;
    const CmHirType *return_type;
    const CmHirGenericArg *argument;
    CmHirTypeAliasResult result;
    size_t arena_bytes;
    size_t interner_length;
    size_t type_count;

    check(build_memory_graph(source, &sources, &graph, &cfg, &imports,
            &graph_result, &import_result),
        "nominal const normalizer could not build its graph");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    lower_result = lower_graph(&hir, &graph, graph_result.revision,
        &imports, &map);
    array = find_item(&hir, "Array");
    owner = find_item(&hir, "Owner");
    make = find_item(&hir, "make");
    array_parameter = array == NULL ? NULL
        : find_generic(&hir, array->definition, CM_HIR_GENERIC_CONST);
    owner_parameter = owner == NULL ? NULL
        : find_generic(&hir, owner->definition, CM_HIR_GENERIC_CONST);
    owner_parameter_id = find_generic_id(&hir, owner_parameter);
    return_type_id = make == NULL ? CM_HIR_TYPE_NONE
        : make->data.function_item.signature.return_type;
    return_type = cm_hir_get_type(&hir, return_type_id);
    argument = return_type == NULL
            || return_type->kind != CM_HIR_TYPE_ADT_KIND
            || return_type->data.named_type.argument_count != 1u
        ? NULL : &return_type->data.named_type.arguments[0];
    check(lower_result.error_count == 0u && array != NULL && owner != NULL
        && make != NULL && array_parameter != NULL
        && owner_parameter != NULL
        && owner_parameter_id != CM_HIR_GENERIC_PARAM_NONE
        && argument != NULL
        && cm_hir_def_id_equal(return_type->data.named_type.definition,
            array->definition)
        && argument->kind == CM_HIR_GENERIC_ARG_CONST
        && argument->data.constant.kind == CM_HIR_CONST_PARAMETER
        && argument->data.constant.type == array_parameter->declared_type
        && argument->data.constant.data.parameter == owner_parameter_id,
        "nominal ADT lost its compatible foreign const parameter");
    if (argument == NULL) goto cleanup;

    type_count = hir.types.len;
    arena_bytes = cm_arena_bytes_used(&hir.storage);
    interner_length = cm_interner_length(&hir.strings);
    result = cm_hir_normalize_type_aliases(&hir, return_type_id);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type == return_type_id
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes
        && cm_interner_length(&hir.strings) == interner_length,
        "nominal ADT rejected or mutated a compatible foreign const parameter");

cleanup:
    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    destroy_graph(&sources, &graph, &imports);
}

static void test_projection_const_parameter_normalization(void)
{
    static const char source[] =
        "trait Copy {} "
        "trait Other<const M: usize> {} "
        "trait Owner<'a, T, const N: usize> { "
        "type Assoc: 'static + Copy + 'a; }";
    CmSourceSet sources;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmImportResolver imports;
    CmModuleGraphResult graph_result;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap map;
    CmHirLowerResult lower_result;
    const CmHirItem *other;
    const CmHirItem *owner;
    const CmHirItem *associated;
    const CmHirGenericParam *other_parameter;
    CmHirGenericParamId other_parameter_id;
    CmHirTypeId projection_id;
    CmHirType *projection;
    CmHirGenericArg *const_argument;
    CmHirConstArg saved_constant;
    CmHirTypeAliasResult result;
    size_t arena_bytes;
    size_t interner_length;
    size_t type_count;

    check(build_memory_graph(source, &sources, &graph, &cfg, &imports,
            &graph_result, &import_result),
        "const identity normalizer could not build its graph");
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&map);
    lower_result = lower_graph(&hir, &graph, graph_result.revision,
        &imports, &map);
    other = find_item(&hir, "Other");
    owner = find_item(&hir, "Owner");
    associated = find_item(&hir, "Assoc");
    other_parameter = other == NULL ? NULL
        : find_generic(&hir, other->definition, CM_HIR_GENERIC_CONST);
    other_parameter_id = find_generic_id(&hir, other_parameter);
    projection_id = associated == NULL
            || associated->outlives_predicate_count == 0u
        ? CM_HIR_TYPE_NONE
        : associated->outlives_predicates[0].subject.type;
    projection = projection_id == CM_HIR_TYPE_NONE ? NULL
        : (CmHirType *)cm_vec_at(&hir.types,
            (size_t)projection_id - 1u);
    const_argument = projection == NULL
            || projection->kind != CM_HIR_TYPE_PROJECTION_KIND
            || projection->data.projection_type.trait_type.argument_count
                != 3u
            || projection->data.projection_type.trait_type.arguments[2].kind
                != CM_HIR_GENERIC_ARG_CONST
        ? NULL : &projection->data.projection_type.trait_type.arguments[2];
    check(lower_result.error_count == 0u && owner != NULL && other != NULL
        && associated != NULL && other_parameter != NULL
        && other_parameter_id != CM_HIR_GENERIC_PARAM_NONE
        && const_argument != NULL,
        "const identity normalizer lost its projection fixture");
    if (const_argument == NULL || projection == NULL || owner == NULL) {
        cm_hir_module_map_destroy(&map);
        cm_hir_context_destroy(&hir);
        destroy_graph(&sources, &graph, &imports);
        return;
    }

    type_count = hir.types.len;
    arena_bytes = cm_arena_bytes_used(&hir.storage);
    interner_length = cm_interner_length(&hir.strings);
    result = cm_hir_normalize_type_aliases(&hir, projection_id);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type == projection_id
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes,
        "authenticated const identity projection did not normalize in place");

    saved_constant = const_argument->data.constant;
    const_argument->data.constant.data.parameter = other_parameter_id;
    result = cm_hir_normalize_type_aliases(&hir, projection_id);
    check(result.status == CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST
        && result.type == CM_HIR_TYPE_NONE
        && result.source_type == projection_id
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes
        && cm_interner_length(&hir.strings) == interner_length,
        "foreign trait const parameter identity escaped normalization");
    const_argument->data.constant = saved_constant;

    const_argument->data.constant.type = projection->data.projection_type
        .trait_type.arguments[1].data.type;
    result = cm_hir_normalize_type_aliases(&hir, projection_id);
    check(result.status == CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST
        && result.type == CM_HIR_TYPE_NONE
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes
        && cm_interner_length(&hir.strings) == interner_length,
        "wrong-typed const parameter identity escaped normalization");
    const_argument->data.constant = saved_constant;

    const_argument->data.constant.kind = CM_HIR_CONST_VALUE;
    const_argument->data.constant.data.value.low_bits = 1u;
    const_argument->data.constant.data.value.high_bits = 0u;
    result = cm_hir_normalize_type_aliases(&hir, projection_id);
    check(result.status == CM_HIR_TYPE_ALIAS_UNSUPPORTED_CONST
        && result.type == CM_HIR_TYPE_NONE
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes
        && cm_interner_length(&hir.strings) == interner_length,
        "const value behavior changed in identity-only normalization");
    const_argument->data.constant = saved_constant;

    cm_hir_module_map_destroy(&map);
    cm_hir_context_destroy(&hir);
    destroy_graph(&sources, &graph, &imports);
}

static void test_dyn_trait_alias_normalization(void)
{
    CmHirContext hir;
    CmSpan span;
    CmHirCrateId crate_id;
    CmHirModuleId root_module;
    CmHirDefId principal_definition;
    CmHirDefId associated_definition;
    CmHirDefId id_definition;
    CmHirDefId owner_definition;
    CmHirGenericParam parameter;
    CmHirGenericParamId principal_lifetime;
    CmHirGenericParamId principal_type;
    CmHirGenericParamId id_parameter;
    CmHirGenericParamId owner_lifetime;
    CmHirGenericParamId owner_type;
    CmHirGenericArg argument;
    CmHirGenericArg principal_arguments[2];
    CmHirGenericArg instantiation_arguments[2];
    CmHirAssociatedTypeEquality equality;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirType type;
    CmHirTypeId u16_type;
    CmHirTypeId id_parameter_type;
    CmHirTypeId owner_parameter_type;
    CmHirTypeId id_application;
    CmHirTypeId dyn_type;
    CmHirTypeId normalized_type;
    CmHirTypeAliasResult result;
    const CmHirType *normalized;
    const CmHirType *original;
    size_t type_count;
    size_t arena_bytes;

    cm_hir_context_init(&hir);
    span.source = 113u;
    span.start = 10u;
    span.end = 90u;
    check(cm_hir_create_crate(&hir,
            cm_hir_intern(&hir, "dyn_alias_api"),
            CM_HIR_EDITION_2024, span, &crate_id, &root_module)
            == CM_HIR_OK,
        "dyn alias setup could not create a crate");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_INTEGER_KIND;
    type.span = span;
    type.data.integer_type.kind = CM_HIR_INT_U16;
    check(cm_hir_add_type(&hir, &type, &u16_type) == CM_HIR_OK,
        "dyn alias setup could not add u16");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &principal_definition) == CM_HIR_OK,
        "dyn alias setup could not reserve its principal trait");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_LIFETIME;
    parameter.owner = principal_definition;
    parameter.name = cm_hir_intern(&hir, "'p");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter,
            &principal_lifetime) == CM_HIR_OK,
        "dyn alias setup could not add its principal lifetime");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = principal_definition;
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&hir, "P");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter,
            &principal_type) == CM_HIR_OK,
        "dyn alias setup could not add its principal type");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TRAIT,
        principal_definition, cm_hir_def_id_none(), root_module,
        "Principal", span);
    item.generic_parameter_start = principal_lifetime;
    item.generic_parameter_count = 2u;
    item.data.trait_item.safety = CM_HIR_SAFE;
    check(principal_type == principal_lifetime + 1u
        && cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "dyn alias setup could not bind its principal trait");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &associated_definition) == CM_HIR_OK,
        "dyn alias setup could not reserve its associated type");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TYPE_ALIAS,
        associated_definition, principal_definition, root_module,
        "Output", span);
    item.data.type_alias_item.target = CM_HIR_TYPE_NONE;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "dyn alias setup could not bind its associated type");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &id_definition) == CM_HIR_OK,
        "dyn alias setup could not reserve Id");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = id_definition;
    parameter.name = cm_hir_intern(&hir, "I");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter, &id_parameter)
            == CM_HIR_OK,
        "dyn alias setup could not add Id's parameter");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = span;
    type.data.parameter_type.parameter = id_parameter;
    check(cm_hir_add_type(&hir, &type, &id_parameter_type) == CM_HIR_OK,
        "dyn alias setup could not add Id's parameter type");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TYPE_ALIAS,
        id_definition, cm_hir_def_id_none(), root_module, "Id", span);
    item.generic_parameter_start = id_parameter;
    item.generic_parameter_count = 1u;
    item.data.type_alias_item.target = id_parameter_type;
    check(cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "dyn alias setup could not bind Id");

    check(cm_hir_reserve_item_definition(&hir, crate_id, span,
            &owner_definition) == CM_HIR_OK,
        "dyn alias setup could not reserve DynAlias");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_LIFETIME;
    parameter.owner = owner_definition;
    parameter.name = cm_hir_intern(&hir, "'a");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter, &owner_lifetime)
            == CM_HIR_OK,
        "dyn alias setup could not add DynAlias's lifetime");
    memset(&parameter, 0, sizeof(parameter));
    parameter.kind = CM_HIR_GENERIC_TYPE;
    parameter.owner = owner_definition;
    parameter.index = 1u;
    parameter.name = cm_hir_intern(&hir, "T");
    parameter.span = span;
    check(cm_hir_add_generic_param(&hir, &parameter, &owner_type)
            == CM_HIR_OK,
        "dyn alias setup could not add DynAlias's type");
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_PARAMETER_KIND;
    type.span = span;
    type.data.parameter_type.parameter = owner_type;
    check(cm_hir_add_type(&hir, &type, &owner_parameter_type)
            == CM_HIR_OK,
        "dyn alias setup could not add DynAlias's parameter type");
    memset(&argument, 0, sizeof(argument));
    argument.kind = CM_HIR_GENERIC_ARG_TYPE;
    argument.data.type = owner_parameter_type;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
    type.span = span;
    type.data.named_type.definition = id_definition;
    type.data.named_type.arguments = &argument;
    type.data.named_type.argument_count = 1u;
    check(cm_hir_add_type(&hir, &type, &id_application) == CM_HIR_OK,
        "dyn alias setup could not add Id<T>");
    memset(principal_arguments, 0, sizeof(principal_arguments));
    principal_arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    principal_arguments[0].data.lifetime.kind =
        CM_HIR_REGION_EARLY_BOUND;
    principal_arguments[0].data.lifetime.data.parameter = owner_lifetime;
    principal_arguments[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    principal_arguments[1].data.type = id_application;
    memset(&type, 0, sizeof(type));
    type.kind = CM_HIR_TYPE_DYN_TRAIT_KIND;
    type.span = span;
    type.data.dyn_trait_type.has_principal = 1;
    type.data.dyn_trait_type.principal_trait.definition =
        principal_definition;
    type.data.dyn_trait_type.principal_trait.arguments =
        principal_arguments;
    type.data.dyn_trait_type.principal_trait.argument_count = 2u;
    memset(&equality, 0, sizeof(equality));
    equality.associated_type = associated_definition;
    equality.value = id_application;
    equality.span = span;
    type.data.dyn_trait_type.equalities = &equality;
    type.data.dyn_trait_type.equality_count = 1u;
    type.data.dyn_trait_type.region.kind = CM_HIR_REGION_EARLY_BOUND;
    type.data.dyn_trait_type.region.data.parameter = owner_lifetime;
    check(cm_hir_add_type(&hir, &type, &dyn_type) == CM_HIR_OK,
        "dyn alias setup could not add its dyn trait type");
    init_projection_test_item(&hir, &item, CM_HIR_ITEM_TYPE_ALIAS,
        owner_definition, cm_hir_def_id_none(), root_module, "DynAlias",
        span);
    item.generic_parameter_start = owner_lifetime;
    item.generic_parameter_count = 2u;
    item.data.type_alias_item.target = dyn_type;
    check(owner_type == owner_lifetime + 1u
        && cm_hir_add_item(&hir, &item, &item_id) == CM_HIR_OK,
        "dyn alias setup could not bind DynAlias");

    original = cm_hir_get_type(&hir, dyn_type);
    result = cm_hir_normalize_type_aliases(&hir, dyn_type);
    normalized = cm_hir_get_type(&hir, result.type);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type != CM_HIR_TYPE_NONE && result.type != dyn_type
        && result.allocated_type_count != 0u
        && normalized != NULL
        && normalized->kind == CM_HIR_TYPE_DYN_TRAIT_KIND
        && cm_hir_def_id_equal(
            normalized->data.dyn_trait_type.principal_trait.definition,
            principal_definition)
        && normalized->data.dyn_trait_type.principal_trait.argument_count
            == 2u
        && normalized->data.dyn_trait_type.principal_trait.arguments[0]
            .kind == CM_HIR_GENERIC_ARG_LIFETIME
        && normalized->data.dyn_trait_type.principal_trait.arguments[0]
            .data.lifetime.kind == CM_HIR_REGION_EARLY_BOUND
        && normalized->data.dyn_trait_type.principal_trait.arguments[0]
            .data.lifetime.data.parameter == owner_lifetime
        && normalized->data.dyn_trait_type.principal_trait.arguments[1]
            .kind == CM_HIR_GENERIC_ARG_TYPE
        && parameter_type_is(&hir,
            normalized->data.dyn_trait_type.principal_trait.arguments[1]
                .data.type,
            owner_type)
        && normalized->data.dyn_trait_type.equalities != NULL
        && normalized->data.dyn_trait_type.equality_count == 1u
        && cm_hir_def_id_equal(normalized->data.dyn_trait_type
                .equalities[0].associated_type,
            associated_definition)
        && parameter_type_is(&hir,
            normalized->data.dyn_trait_type.equalities[0].value,
            owner_type)
        && normalized->data.dyn_trait_type.region.kind
            == CM_HIR_REGION_EARLY_BOUND
        && normalized->data.dyn_trait_type.region.data.parameter
            == owner_lifetime
        && original != NULL
        && original->data.dyn_trait_type.principal_trait.arguments[1]
            .data.type == id_application,
        "dyn trait normalization lost its principal arguments, region, or source immutability");
    normalized_type = result.type;

    memset(instantiation_arguments, 0, sizeof(instantiation_arguments));
    instantiation_arguments[0].kind = CM_HIR_GENERIC_ARG_LIFETIME;
    instantiation_arguments[0].data.lifetime.kind = CM_HIR_REGION_STATIC;
    instantiation_arguments[1].kind = CM_HIR_GENERIC_ARG_TYPE;
    instantiation_arguments[1].data.type = u16_type;
    result = cm_hir_instantiate_type(&hir, normalized_type,
        owner_definition, instantiation_arguments, 2u);
    normalized = cm_hir_get_type(&hir, result.type);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type != CM_HIR_TYPE_NONE
        && normalized != NULL
        && normalized->kind == CM_HIR_TYPE_DYN_TRAIT_KIND
        && normalized->data.dyn_trait_type.principal_trait.arguments[0]
            .data.lifetime.kind == CM_HIR_REGION_STATIC
        && type_is_integer(&hir,
            normalized->data.dyn_trait_type.principal_trait.arguments[1]
                .data.type,
            CM_HIR_INT_U16)
        && normalized->data.dyn_trait_type.equality_count == 1u
        && type_is_integer(&hir,
            normalized->data.dyn_trait_type.equalities[0].value,
            CM_HIR_INT_U16)
        && normalized->data.dyn_trait_type.region.kind
            == CM_HIR_REGION_STATIC,
        "dyn trait instantiation did not substitute its principal arguments and exact region");

    normalized_type = result.type;
    type_count = hir.types.len;
    arena_bytes = cm_arena_bytes_used(&hir.storage);
    result = cm_hir_normalize_type_aliases(&hir, normalized_type);
    check(result.status == CM_HIR_TYPE_ALIAS_OK
        && result.type == normalized_type
        && result.allocated_type_count == 0u
        && hir.types.len == type_count
        && cm_arena_bytes_used(&hir.storage) == arena_bytes,
        "unchanged dyn trait normalization grew the type arena");
    cm_hir_context_destroy(&hir);
}

int main(void)
{
    test_structural_aliases();
    test_trailing_type_defaults();
    test_imported_reexported_alias();
    test_generated_alias();
    test_failures_are_transactional();
    test_projection_normalization();
    test_normalizer_api_transaction();
    test_type_instantiation_api();
    test_symbolic_self_normalization();
    test_nominal_const_parameter_normalization();
    test_projection_const_parameter_normalization();
    test_dyn_trait_alias_normalization();
    if (failures != 0) return 1;
    puts("HIR type alias tests: ok");
    return 0;
}
