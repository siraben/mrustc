#include "cm/hir/executable_materialize.h"

#include "cm/alloc.h"
#include "cm/hir/body.h"
#include "library_internal.h"

#include <string.h>

typedef struct CmExecRuntime {
    CmHirModuleId *modules;
    CmHirDefId *traits;
    CmHirDefId *impls;
    CmHirDefId *values;
    CmHirGenericParamId *generics;
    CmHirTypeId *types;
} CmExecRuntime;

static CmHirExecutableMaterializeResult cm_exec_result(
    CmHirExecutableMaterializeStatus status)
{
    CmHirExecutableMaterializeResult result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.metadata_status = CM_HIR_EXEC_METADATA_OK;
    result.hir_status = CM_HIR_OK;
    result.library_status = CM_HIR_LIBRARY_OK;
    return result;
}

static CmSpan cm_exec_span(CmSourceId source, uint32_t ordinal)
{
    CmSpan span;
    span.source = source;
    span.start = ordinal;
    span.end = ordinal == UINT32_MAX ? UINT32_MAX : ordinal + 1u;
    return span;
}

static CmInternId cm_exec_intern(CmHirContext *context,
    CmHirExecutableString string)
{
    return cm_interner_intern(&context->strings, string.data, string.length);
}

static int cm_exec_edition(uint32_t edition, CmHirEdition *out)
{
    switch (edition) {
    case UINT32_C(2015): *out = CM_HIR_EDITION_2015; return 1;
    case UINT32_C(2018): *out = CM_HIR_EDITION_2018; return 1;
    case UINT32_C(2021): *out = CM_HIR_EDITION_2021; return 1;
    case UINT32_C(2024): *out = CM_HIR_EDITION_2024; return 1;
    }
    return 0;
}

static int cm_exec_primitive(uint8_t primitive, CmHirType *type)
{
    memset(type, 0, sizeof(*type));
    switch (primitive) {
    case CM_HIR_EXEC_PRIMITIVE_BOOL:
        type->kind = CM_HIR_TYPE_BOOL_KIND;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_I8:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_I8;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_U8:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_U8;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_I16:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_I16;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_U16:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_U16;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_I32:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_I32;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_U32:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_U32;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_I64:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_I64;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_U64:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_U64;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_ISIZE:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_ISIZE;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_USIZE:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_USIZE;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_F32:
        type->kind = CM_HIR_TYPE_FLOAT_KIND;
        type->data.float_type.kind = CM_HIR_FLOAT_F32;
        return 1;
    case CM_HIR_EXEC_PRIMITIVE_F64:
        type->kind = CM_HIR_TYPE_FLOAT_KIND;
        type->data.float_type.kind = CM_HIR_FLOAT_F64;
        return 1;
    }
    return 0;
}

static void cm_exec_runtime_destroy(CmExecRuntime *runtime)
{
    cm_free(runtime->modules);
    cm_free(runtime->traits);
    cm_free(runtime->impls);
    cm_free(runtime->values);
    cm_free(runtime->generics);
    cm_free(runtime->types);
    memset(runtime, 0, sizeof(*runtime));
}

static int cm_exec_runtime_init(CmExecRuntime *runtime,
    const CmHirExecutableMetadata *metadata)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->modules = cm_alloc_zeroed(metadata->module_count,
        sizeof(*runtime->modules));
    runtime->traits = cm_alloc_zeroed(metadata->trait_count,
        sizeof(*runtime->traits));
    runtime->impls = cm_alloc_zeroed(metadata->impl_count,
        sizeof(*runtime->impls));
    runtime->values = cm_alloc_zeroed(metadata->value_count,
        sizeof(*runtime->values));
    runtime->generics = cm_alloc_zeroed(metadata->value_count,
        sizeof(*runtime->generics));
    runtime->types = cm_alloc_zeroed(metadata->type_count,
        sizeof(*runtime->types));
    return runtime->modules != NULL && runtime->traits != NULL
        && runtime->impls != NULL && runtime->values != NULL
        && runtime->generics != NULL && runtime->types != NULL;
}

static CmHirStatus cm_exec_reserve_definitions(CmHirContext *context,
    CmHirCrateId crate_id, const CmHirExecutableMetadata *metadata,
    CmExecRuntime *runtime, CmSpan span)
{
    size_t index;
    CmHirStatus status;
    for (index = 0u; index < metadata->trait_count; ++index) {
        status = cm_hir_reserve_item_definition_as(context, crate_id,
            CM_HIR_ITEM_TRAIT, span, &runtime->traits[index]);
        if (status != CM_HIR_OK) return status;
    }
    for (index = 0u; index < metadata->impl_count; ++index) {
        status = cm_hir_reserve_item_definition_as(context, crate_id,
            CM_HIR_ITEM_IMPL, span, &runtime->impls[index]);
        if (status != CM_HIR_OK) return status;
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        status = cm_hir_reserve_item_definition_as(context, crate_id,
            CM_HIR_ITEM_FUNCTION, span, &runtime->values[index]);
        if (status != CM_HIR_OK) return status;
    }
    return CM_HIR_OK;
}

static CmHirStatus cm_exec_add_generics_and_types(CmHirContext *context,
    const CmHirExecutableMetadata *metadata, CmExecRuntime *runtime,
    CmSourceId source)
{
    size_t index;
    CmHirStatus status;
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirExecutableValue *value = &metadata->values[index];
        CmHirGenericParam parameter;
        if (value->kind != CM_HIR_EXEC_VALUE_RECIPE) continue;
        memset(&parameter, 0, sizeof(parameter));
        parameter.kind = CM_HIR_GENERIC_TYPE;
        parameter.owner = runtime->values[index];
        parameter.index = 0u;
        parameter.name = cm_exec_intern(context, value->generic_name);
        parameter.span = cm_exec_span(source, value->source_ordinal);
        status = cm_hir_add_generic_param(context, &parameter,
            &runtime->generics[index]);
        if (status != CM_HIR_OK) return status;
    }
    for (index = 0u; index < metadata->type_count; ++index) {
        const CmHirExecutableType *wire = &metadata->types[index];
        CmHirType type;
        if (wire->kind == CM_HIR_EXEC_TYPE_PRIMITIVE) {
            if (!cm_exec_primitive(wire->primitive, &type))
                return CM_HIR_INVARIANT_VIOLATION;
        } else {
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_PARAMETER_KIND;
            type.data.parameter_type.parameter =
                runtime->generics[wire->owner_value - 1u];
        }
        type.span = cm_exec_span(source, (uint32_t)(index + 1u));
        status = cm_hir_add_type(context, &type, &runtime->types[index]);
        if (status != CM_HIR_OK) return status;
    }
    return CM_HIR_OK;
}

static CmHirStatus cm_exec_bind_traits_and_impls(CmHirContext *context,
    const CmHirExecutableMetadata *metadata, const CmExecRuntime *runtime,
    CmSourceId source)
{
    size_t index;
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirExecutableTrait *wire = &metadata->traits[index];
        CmHirItem item;
        CmHirItemId item_id;
        CmHirStatus status;
        memset(&item, 0, sizeof(item));
        item.kind = CM_HIR_ITEM_TRAIT;
        item.definition = runtime->traits[index];
        item.owner_module = runtime->modules[wire->owner_module - 1u];
        item.parent_definition = cm_hir_def_id_none();
        item.name = cm_exec_intern(context, wire->name);
        item.visibility.kind = CM_HIR_VIS_PUBLIC;
        item.visibility.restriction = cm_hir_def_id_none();
        item.span = cm_exec_span(source, wire->source_ordinal);
        item.data.trait_item.safety = CM_HIR_SAFE;
        status = cm_hir_add_item(context, &item, &item_id);
        if (status != CM_HIR_OK) return status;
    }
    for (index = 0u; index < metadata->impl_count; ++index) {
        const CmHirExecutableImpl *wire = &metadata->impls[index];
        CmHirItem item;
        CmHirItemId item_id;
        CmHirStatus status;
        memset(&item, 0, sizeof(item));
        item.kind = CM_HIR_ITEM_IMPL;
        item.definition = runtime->impls[index];
        item.owner_module = runtime->modules[wire->owner_module - 1u];
        item.parent_definition = cm_hir_def_id_none();
        item.visibility.kind = CM_HIR_VIS_PRIVATE;
        item.visibility.restriction = cm_hir_def_id_none();
        item.span = cm_exec_span(source, wire->source_ordinal);
        item.data.impl_item.self_type = runtime->types[wire->self_type - 1u];
        item.data.impl_item.has_trait = 1;
        item.data.impl_item.trait_type.definition =
            runtime->traits[wire->trait_local - 1u];
        item.data.impl_item.safety = CM_HIR_SAFE;
        item.data.impl_item.polarity = CM_HIR_IMPL_POSITIVE;
        status = cm_hir_add_item(context, &item, &item_id);
        if (status != CM_HIR_OK) return status;
    }
    return CM_HIR_OK;
}

static CmHirStatus cm_exec_bind_value(CmHirContext *context,
    const CmHirExecutableMetadata *metadata, const CmExecRuntime *runtime,
    size_t value_index, CmSourceId source)
{
    const CmHirExecutableValue *wire = &metadata->values[value_index];
    CmHirFunctionParameter *parameters;
    CmHirLocal *locals;
    CmHirTraitPredicate *predicates;
    CmHirBodyId body_id = CM_HIR_BODY_NONE;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirStatus status = CM_HIR_OK;
    size_t index;

    parameters = cm_alloc_zeroed(wire->parameter_count, sizeof(*parameters));
    locals = wire->kind == CM_HIR_EXEC_VALUE_RECIPE
        ? cm_alloc_zeroed(wire->parameter_count, sizeof(*locals)) : NULL;
    predicates = wire->predicate_count == 0u ? NULL
        : cm_alloc_zeroed(wire->predicate_count, sizeof(*predicates));
    if (parameters == NULL
        || (wire->kind == CM_HIR_EXEC_VALUE_RECIPE && locals == NULL)
        || (wire->predicate_count != 0u && predicates == NULL)) {
        status = CM_HIR_INVALID_ARGUMENT;
        goto done;
    }
    for (index = 0u; index < wire->parameter_count; ++index) {
        unsigned char local_name[32];
        size_t length;
        uint32_t value = (uint32_t)index;
        unsigned char reversed[10];
        size_t digits = 0u;
        do {
            reversed[digits++] = (unsigned char)('0' + value % 10u);
            value /= 10u;
        } while (value != 0u);
        memcpy(local_name, "__cm_arg", 8u);
        length = 8u;
        while (digits != 0u) local_name[length++] = reversed[--digits];
        parameters[index].name = cm_interner_intern(&context->strings,
            local_name, length);
        parameters[index].type = runtime->types[wire->parameter_types[index]
            - 1u];
        parameters[index].span = cm_exec_span(source,
            wire->source_ordinal);
        parameters[index].binding_kind = CM_HIR_BINDING_NAMED;
        parameters[index].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
        if (locals != NULL) {
            locals[index].name = parameters[index].name;
            locals[index].type = parameters[index].type;
            locals[index].mutability = CM_HIR_IMMUTABLE;
            locals[index].span = parameters[index].span;
            locals[index].parameter_index = (uint32_t)index;
            locals[index].parameter_binding_index = 0u;
        }
    }
    for (index = 0u; index < wire->predicate_count; ++index) {
        const CmHirExecutablePredicate *predicate = &metadata->predicates[
            wire->predicate_start - 1u + index];
        predicates[index].subject = runtime->types[
            predicate->subject_type - 1u];
        predicates[index].trait_type.definition = runtime->traits[
            predicate->trait_local - 1u];
        predicates[index].scope = CM_HIR_PREDICATE_SCOPE_NONE;
        predicates[index].span = cm_exec_span(source, wire->source_ordinal);
        predicates[index].modifier = CM_HIR_PREDICATE_REQUIRED;
    }
    if (wire->kind == CM_HIR_EXEC_VALUE_RECIPE) {
        const CmHirExecutableBody *recipe = &metadata->bodies[
            wire->execution_local - 1u];
        CmHirBody body;
        CmHirExpr expression;
        CmHirExprId local_expression;
        CmHirExprId block_expression;
        memset(&body, 0, sizeof(body));
        body.owner = runtime->values[value_index];
        body.origin = cm_hir_body_origin_metadata_recipe(
            runtime->values[value_index], metadata->artifact_identity.bytes,
            wire->execution_local, recipe->parameter_index);
        body.state = CM_HIR_BODY_UNLOWERED;
        body.expected_type = runtime->types[wire->return_type - 1u];
        body.locals = locals;
        body.local_count = wire->parameter_count;
        body.parameter_count = wire->parameter_count;
        body.source = source;
        body.source_expression_id = 0u;
        body.span = cm_exec_span(source, wire->source_ordinal);
        status = cm_hir_add_body(context, &body, &body_id);
        if (status != CM_HIR_OK) goto done;
        status = cm_hir_body_add_local_expression(context, body_id,
            recipe->parameter_index, runtime->types[recipe->parameter_type - 1u],
            body.span, &local_expression);
        if (status != CM_HIR_OK) goto done;
        memset(&expression, 0, sizeof(expression));
        expression.kind = CM_HIR_EXPR_BLOCK;
        expression.owner_body = body_id;
        expression.type = runtime->types[recipe->return_type - 1u];
        expression.span = body.span;
        expression.usage = CM_HIR_USAGE_UNKNOWN;
        expression.static_borrow_state = CM_HIR_STATIC_BORROW_UNKNOWN;
        expression.data.block.tail_expression = local_expression;
        status = cm_hir_add_expr(context, &expression, &block_expression);
        if (status != CM_HIR_OK) goto done;
        status = cm_hir_set_body_root_expression(context, body_id,
            block_expression);
        if (status != CM_HIR_OK) goto done;
    }
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = runtime->values[value_index];
    item.owner_module = runtime->modules[wire->owner_module - 1u];
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_exec_intern(context, wire->name);
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = cm_exec_span(source, wire->source_ordinal);
    item.generic_parameter_start = wire->kind == CM_HIR_EXEC_VALUE_RECIPE
        ? runtime->generics[value_index] : CM_HIR_GENERIC_PARAM_NONE;
    item.generic_parameter_count = wire->kind == CM_HIR_EXEC_VALUE_RECIPE
        ? 1u : 0u;
    item.predicates = predicates;
    item.predicate_count = wire->predicate_count;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = wire->parameter_count;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type =
        runtime->types[wire->return_type - 1u];
    item.data.function_item.signature.abi = cm_hir_intern(context,
        wire->kind == CM_HIR_EXEC_VALUE_RECIPE ? "Rust" : "C");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = body_id;
    status = cm_hir_add_item(context, &item, &item_id);
done:
    cm_free(predicates);
    cm_free(locals);
    cm_free(parameters);
    return status;
}

static CmHirLibraryStatus cm_exec_add_library_value(CmHirContext *context,
    CmHirLibraryOwnedData *owned, const CmHirExecutableMetadata *metadata,
    const CmExecRuntime *runtime, size_t value_index, CmSourceId source)
{
    const CmHirExecutableValue *wire = &metadata->values[value_index];
    CmHirLibraryValue value;
    CmHirTypeId *parameters;
    CmHirTraitPredicate *predicates;
    CmHirLibraryNominalReference *references;
    uint32_t reference_count = 0u;
    size_t index;
    CmHirLibraryStatus status;
    memset(&value, 0, sizeof(value));
    parameters = cm_alloc_zeroed(wire->parameter_count, sizeof(*parameters));
    predicates = wire->predicate_count == 0u ? NULL
        : cm_alloc_zeroed(wire->predicate_count, sizeof(*predicates));
    references = wire->predicate_count == 0u ? NULL
        : cm_alloc_zeroed(metadata->trait_count, sizeof(*references));
    if (parameters == NULL
        || (wire->predicate_count != 0u
            && (predicates == NULL || references == NULL))) {
        status = CM_HIR_LIBRARY_INVALID_ARGUMENT;
        goto done;
    }
    for (index = 0u; index < wire->parameter_count; ++index)
        parameters[index] = runtime->types[wire->parameter_types[index] - 1u];
    for (index = 0u; index < wire->predicate_count; ++index) {
        const CmHirExecutablePredicate *predicate = &metadata->predicates[
            wire->predicate_start - 1u + index];
        predicates[index].subject = runtime->types[
            predicate->subject_type - 1u];
        predicates[index].trait_type.definition = runtime->traits[
            predicate->trait_local - 1u];
        predicates[index].scope = CM_HIR_PREDICATE_SCOPE_NONE;
        predicates[index].span = cm_exec_span(source, wire->source_ordinal);
        predicates[index].modifier = CM_HIR_PREDICATE_REQUIRED;
    }
    for (index = 0u; index < metadata->trait_count; ++index) {
        size_t child;
        int referenced = 0;
        for (child = 0u; child < wire->predicate_count; ++child) {
            const CmHirExecutablePredicate *predicate = &metadata->predicates[
                wire->predicate_start - 1u + child];
            if (predicate->trait_local == index + 1u) referenced = 1;
        }
        if (referenced) {
            const CmHirExecutableTrait *trait = &metadata->traits[index];
            const CmHirModule *owner = cm_hir_get_module(context,
                runtime->modules[trait->owner_module - 1u]);
            CmHirLibraryNominalReference *reference =
                &references[reference_count++];
            reference->definition = runtime->traits[index];
            reference->owner_module = owner->definition;
            reference->name.bytes = trait->name.data;
            reference->name.length = trait->name.length;
            reference->use = CM_HIR_LIBRARY_REFERENCE_ONLY;
            reference->kind = CM_HIR_LIBRARY_NOMINAL_TRAIT;
            reference->declaring_trait = cm_hir_def_id_none();
        }
    }
    value.definition = runtime->values[value_index];
    value.kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    value.data.function.parameter_types = parameters;
    value.data.function.parameter_count = wire->parameter_count;
    value.data.function.return_type = runtime->types[wire->return_type - 1u];
    value.data.function.generic_parameter_start =
        wire->kind == CM_HIR_EXEC_VALUE_RECIPE
            ? runtime->generics[value_index] : CM_HIR_GENERIC_PARAM_NONE;
    value.data.function.generic_parameter_count =
        wire->kind == CM_HIR_EXEC_VALUE_RECIPE ? 1u : 0u;
    value.data.function.predicates = predicates;
    value.data.function.predicate_count = wire->predicate_count;
    value.data.function.nominal_references = references;
    value.data.function.nominal_reference_count = reference_count;
    value.data.function.abi = cm_hir_intern(context,
        wire->kind == CM_HIR_EXEC_VALUE_RECIPE ? "Rust" : "C");
    value.data.function.safety = CM_HIR_SAFE;
    status = cm_hir_library_owned_data_add_value(owned, &value);
done:
    cm_free(references);
    cm_free(predicates);
    cm_free(parameters);
    return status;
}

static CmHirLibraryStatus cm_exec_build_owned(CmHirContext *context,
    CmHirLibraryOwnedData *owned, const CmHirExecutableMetadata *metadata,
    const CmExecRuntime *runtime, CmSourceId source)
{
    size_t index;
    for (index = 0u; index < metadata->module_count; ++index) {
        const CmHirModule *module = cm_hir_get_module(context,
            runtime->modules[index]);
        size_t added;
        CmHirLibraryStatus status = cm_hir_library_owned_data_add_module(
            owned, module->definition, &added);
        if (status != CM_HIR_LIBRARY_OK || added != index)
            return status == CM_HIR_LIBRARY_OK
                ? CM_HIR_LIBRARY_INVALID_HIR : status;
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        CmHirLibraryStatus status = cm_exec_add_library_value(context, owned,
            metadata, runtime, index, source);
        if (status != CM_HIR_LIBRARY_OK) return status;
    }
    for (index = 0u; index < metadata->namespace_count; ++index) {
        const CmHirExecutableNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        CmHirLibraryBinding binding;
        CmHirLibraryStatus status;
        memset(&binding, 0, sizeof(binding));
        binding.type_kind = CM_HIR_TYPE_ERROR_KIND;
        binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
        binding.value_kind = CM_HIR_LIBRARY_VALUE_NONE;
        if (entry->target_kind == CM_HIR_EXEC_NAMESPACE_MODULE) {
            const CmHirModule *target = cm_hir_get_module(context,
                runtime->modules[entry->target_local - 1u]);
            binding.kind = CM_HIR_LIBRARY_BINDING_MODULE;
            binding.definition = target->definition;
        } else if (entry->target_kind == CM_HIR_EXEC_NAMESPACE_TRAIT) {
            binding.kind = CM_HIR_LIBRARY_BINDING_TRAIT;
            binding.definition = runtime->traits[entry->target_local - 1u];
        } else {
            binding.kind = CM_HIR_LIBRARY_BINDING_VALUE;
            binding.definition = runtime->values[entry->target_local - 1u];
            binding.value_kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
        }
        status = cm_hir_library_owned_data_add_entry(owned,
            entry->owner_module - 1u, entry->name.data, entry->name.length,
            &binding);
        if (status != CM_HIR_LIBRARY_OK) return status;
    }
    return CM_HIR_LIBRARY_OK;
}

CmHirExecutableMaterializeResult cm_hir_executable_metadata_materialize(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const CmHirExecutableMetadata *metadata, const char *extern_name,
    CmSourceId metadata_source)
{
    CmHirExecutableMaterializeResult result = cm_exec_result(
        CM_HIR_EXEC_MATERIALIZE_INVALID_ARGUMENT);
    CmHirExecutableMetadataStatus metadata_status;
    CmHirEdition edition;
    CmExecRuntime runtime;
    CmHirContextMark mark;
    CmHirLibraryOwnedData owned;
    CmHirLibraryArtifact candidate;
    CmHirCrateId crate_id = CM_HIR_CRATE_NONE;
    CmHirModuleId root_module = CM_HIR_MODULE_NONE;
    CmSpan span;
    size_t index;
    int mark_active = 0;
    int owned_active = 0;
    int candidate_active = 0;

    memset(&runtime, 0, sizeof(runtime));
    memset(&mark, 0, sizeof(mark));
    memset(&owned, 0, sizeof(owned));
    memset(&candidate, 0, sizeof(candidate));
    if (context == NULL || artifact == NULL || metadata == NULL
        || extern_name == NULL || extern_name[0] == '\0'
        || metadata_source == 0u) return result;
    metadata_status = cm_hir_executable_metadata_validate(metadata);
    if (metadata_status != CM_HIR_EXEC_METADATA_OK) {
        result.status = CM_HIR_EXEC_MATERIALIZE_INVALID_METADATA;
        result.metadata_status = metadata_status;
        return result;
    }
    if (!cm_exec_edition(metadata->edition, &edition)
        || !cm_exec_runtime_init(&runtime, metadata)) {
        result.status = CM_HIR_EXEC_MATERIALIZE_HIR_FAILURE;
        result.hir_status = CM_HIR_INVALID_ARGUMENT;
        goto cleanup;
    }
    if (cm_hir_context_mark(context, &mark) != CM_HIR_OK) {
        result.status = CM_HIR_EXEC_MATERIALIZE_HIR_FAILURE;
        result.hir_status = CM_HIR_INVALID_ARGUMENT;
        goto cleanup;
    }
    mark_active = 1;
    span = cm_exec_span(metadata_source, 0u);
    result.hir_status = cm_hir_create_crate(context,
        cm_exec_intern(context, metadata->crate_name), edition, span,
        &crate_id, &root_module);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    runtime.modules[0] = root_module;
    for (index = 1u; index < metadata->module_count; ++index) {
        const CmHirExecutableModule *module = &metadata->modules[index];
        result.hir_status = cm_hir_add_module(context, crate_id,
            runtime.modules[module->parent_module - 1u],
            cm_exec_intern(context, module->name),
            cm_exec_span(metadata_source, (uint32_t)index),
            &runtime.modules[index]);
        if (result.hir_status != CM_HIR_OK) goto hir_failure;
    }
    result.hir_status = cm_exec_reserve_definitions(context, crate_id,
        metadata, &runtime, span);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    result.hir_status = cm_exec_add_generics_and_types(context, metadata,
        &runtime, metadata_source);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    result.hir_status = cm_exec_bind_traits_and_impls(context, metadata,
        &runtime, metadata_source);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    for (index = 0u; index < metadata->value_count; ++index) {
        result.hir_status = cm_exec_bind_value(context, metadata, &runtime,
            index, metadata_source);
        if (result.hir_status != CM_HIR_OK) goto hir_failure;
    }
    cm_hir_library_owned_data_init(&owned);
    owned_active = 1;
    result.library_status = cm_exec_build_owned(context, &owned, metadata,
        &runtime, metadata_source);
    if (result.library_status != CM_HIR_LIBRARY_OK) goto artifact_failure;
    cm_hir_library_artifact_init(&candidate);
    candidate_active = 1;
    {
        const CmHirModule *root = cm_hir_get_module(context, root_module);
        CmHirLibraryArtifactResult restored =
            cm_hir_library_artifact_restore_owned(&candidate, context,
                crate_id, root->definition, extern_name, &owned);
        result.library_status = restored.status;
        if (restored.status != CM_HIR_LIBRARY_OK) goto artifact_failure;
        result.module_count = restored.module_count;
        result.public_type_entry_count = restored.public_type_entry_count;
        result.public_value_entry_count = restored.public_value_entry_count;
    }
    result.hir_status = cm_hir_context_commit(context, &mark);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    mark_active = 0;
    {
        CmHirLibraryArtifact previous = *artifact;
        *artifact = candidate;
        candidate.state = NULL;
        candidate_active = 0;
        cm_hir_library_artifact_destroy(&previous);
    }
    result.status = CM_HIR_EXEC_MATERIALIZE_OK;
    result.crate_id = crate_id;
    result.root_module = root_module;
    goto cleanup;

hir_failure:
    result.status = CM_HIR_EXEC_MATERIALIZE_HIR_FAILURE;
    goto rollback;
artifact_failure:
    result.status = CM_HIR_EXEC_MATERIALIZE_ARTIFACT_FAILURE;
rollback:
    if (mark_active) {
        (void)cm_hir_context_rewind(context, &mark);
        mark_active = 0;
    }
cleanup:
    if (candidate_active) cm_hir_library_artifact_destroy(&candidate);
    if (owned_active) cm_hir_library_owned_data_destroy(&owned);
    cm_exec_runtime_destroy(&runtime);
    return result;
}

const char *cm_hir_executable_materialize_status_name(
    CmHirExecutableMaterializeStatus status)
{
    switch (status) {
    case CM_HIR_EXEC_MATERIALIZE_OK: return "ok";
    case CM_HIR_EXEC_MATERIALIZE_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_EXEC_MATERIALIZE_INVALID_METADATA: return "invalid metadata";
    case CM_HIR_EXEC_MATERIALIZE_HIR_FAILURE: return "HIR failure";
    case CM_HIR_EXEC_MATERIALIZE_ARTIFACT_FAILURE:
        return "artifact failure";
    }
    return "unknown executable materialization status";
}
