#include "cm/hir/declaration_materialize.h"

#include "cm/alloc.h"
#include "library_internal.h"

#include <string.h>

typedef struct CmDeclRuntime {
    size_t item_count;
    CmHirModuleId *modules;
    CmHirDefId *traits;
    CmHirDefId *associated;
    CmHirDefId *items;
    CmHirDefId **variants;
    CmHirDefId *values;
    CmHirGenericParamId *generics;
    CmHirTypeId *types;
} CmDeclRuntime;

static CmHirDeclarationMaterializeResult cm_decl_result(
    CmHirDeclarationMaterializeStatus status)
{
    CmHirDeclarationMaterializeResult result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.metadata_status = CM_HIR_DECL_METADATA_OK;
    result.hir_status = CM_HIR_OK;
    result.library_status = CM_HIR_LIBRARY_OK;
    return result;
}

static CmSpan cm_decl_span(CmSourceId source, uint32_t ordinal)
{
    CmSpan span;
    span.source = source;
    span.start = ordinal;
    span.end = ordinal == UINT32_MAX ? UINT32_MAX : ordinal + 1u;
    return span;
}

static CmInternId cm_decl_intern(CmHirContext *context,
    CmHirDeclarationString string)
{
    return cm_interner_intern(&context->strings, string.data, string.length);
}

static int cm_decl_string_equal(CmHirDeclarationString left,
    CmHirDeclarationString right)
{
    return left.length == right.length
        && (left.length == 0u
            || memcmp(left.data, right.data, left.length) == 0);
}

static int cm_decl_expectation_matches(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationMaterializeExpectation *expectation)
{
    size_t index;
    if (expectation == NULL
        || !cm_decl_string_equal(metadata->crate_name,
            expectation->crate_name)
        || !cm_decl_string_equal(metadata->crate_disambiguator,
            expectation->crate_disambiguator)
        || metadata->edition != expectation->edition
        || !cm_decl_string_equal(metadata->target_triple,
            expectation->target_triple)
        || !cm_decl_string_equal(metadata->data_layout,
            expectation->data_layout)
        || metadata->panic_strategy != expectation->panic_strategy
        || metadata->cfg_count != expectation->cfg_count
        || (expectation->cfg_count != 0u && expectation->cfgs == NULL)) {
        return 0;
    }
    for (index = 0u; index < metadata->cfg_count; ++index) {
        if (!cm_decl_string_equal(metadata->cfgs[index],
                expectation->cfgs[index])) return 0;
    }
    return 1;
}

static int cm_decl_edition(uint8_t edition, CmHirEdition *out)
{
    switch (edition) {
    case CM_HIR_DECL_EDITION_2015: *out = CM_HIR_EDITION_2015; return 1;
    case CM_HIR_DECL_EDITION_2018: *out = CM_HIR_EDITION_2018; return 1;
    case CM_HIR_DECL_EDITION_2021: *out = CM_HIR_EDITION_2021; return 1;
    case CM_HIR_DECL_EDITION_2024: *out = CM_HIR_EDITION_2024; return 1;
    }
    return 0;
}

static void *cm_decl_array(size_t count, size_t element_size)
{
    return count == 0u ? NULL : cm_alloc_zeroed(count, element_size);
}

static int cm_decl_runtime_init(CmDeclRuntime *runtime,
    const CmHirDeclarationMetadata *metadata)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->item_count = metadata->item_count;
    runtime->modules = (CmHirModuleId *)cm_decl_array(metadata->module_count,
        sizeof(*runtime->modules));
    runtime->traits = (CmHirDefId *)cm_decl_array(metadata->trait_count,
        sizeof(*runtime->traits));
    runtime->associated = (CmHirDefId *)cm_decl_array(
        metadata->associated_count, sizeof(*runtime->associated));
    runtime->items = (CmHirDefId *)cm_decl_array(metadata->item_count,
        sizeof(*runtime->items));
    runtime->variants = (CmHirDefId **)cm_decl_array(metadata->item_count,
        sizeof(*runtime->variants));
    runtime->values = (CmHirDefId *)cm_decl_array(metadata->value_count,
        sizeof(*runtime->values));
    runtime->generics = (CmHirGenericParamId *)cm_decl_array(
        metadata->generic_count, sizeof(*runtime->generics));
    runtime->types = (CmHirTypeId *)cm_decl_array(metadata->type_count,
        sizeof(*runtime->types));
    if (!((metadata->module_count == 0u || runtime->modules != NULL)
        && (metadata->trait_count == 0u || runtime->traits != NULL)
        && (metadata->associated_count == 0u
            || runtime->associated != NULL)
        && (metadata->item_count == 0u
            || (runtime->items != NULL && runtime->variants != NULL))
        && (metadata->value_count == 0u || runtime->values != NULL)
        && (metadata->generic_count == 0u || runtime->generics != NULL)
        && (metadata->type_count == 0u || runtime->types != NULL))) return 0;
    {
        size_t index;

        for (index = 0u; index < metadata->item_count; ++index) {
            if (metadata->items[index].kind != CM_HIR_DECL_ITEM_ENUM)
                continue;
            runtime->variants[index] = (CmHirDefId *)cm_decl_array(
                metadata->items[index].variant_count,
                sizeof(*runtime->variants[index]));
            if (metadata->items[index].variant_count != 0u
                && runtime->variants[index] == NULL) return 0;
        }
    }
    return 1;
}

static void cm_decl_runtime_destroy(CmDeclRuntime *runtime)
{
    size_t index;

    if (runtime->variants != NULL) {
        for (index = 0u; runtime->items != NULL
                && index < runtime->item_count; ++index) {
            cm_free(runtime->variants[index]);
        }
    }
    cm_free(runtime->variants);
    cm_free(runtime->types);
    cm_free(runtime->generics);
    cm_free(runtime->values);
    cm_free(runtime->items);
    cm_free(runtime->associated);
    cm_free(runtime->traits);
    cm_free(runtime->modules);
    memset(runtime, 0, sizeof(*runtime));
}

static int cm_decl_runtime_variant(
    const CmHirDeclarationMetadata *metadata, const CmDeclRuntime *runtime,
    uint32_t variant_local, CmHirDefId *out_enum_definition,
    CmHirDefId *out_variant_definition, uint32_t *out_variant_index)
{
    size_t item_index;
    size_t cursor;

    if (metadata == NULL || runtime == NULL || variant_local == 0u
        || out_enum_definition == NULL || out_variant_definition == NULL
        || out_variant_index == NULL) return 0;
    cursor = 1u;
    for (item_index = 0u; item_index < metadata->item_count; ++item_index) {
        const CmHirDeclarationItem *item = &metadata->items[item_index];
        size_t next;

        if (item->kind != CM_HIR_DECL_ITEM_ENUM) continue;
        if ((size_t)item->variant_count > SIZE_MAX - cursor) return 0;
        next = cursor + (size_t)item->variant_count;
        if ((size_t)variant_local >= cursor
            && (size_t)variant_local < next) {
            uint32_t variant_index = (uint32_t)((size_t)variant_local
                - cursor);
            if (runtime->items == NULL || runtime->variants == NULL
                || runtime->variants[item_index] == NULL
                || variant_index >= item->variant_count
                || cm_hir_def_id_is_none(runtime->items[item_index])
                || cm_hir_def_id_is_none(
                    runtime->variants[item_index][variant_index])) return 0;
            *out_enum_definition = runtime->items[item_index];
            *out_variant_definition =
                runtime->variants[item_index][variant_index];
            *out_variant_index = variant_index;
            return 1;
        }
        cursor = next;
    }
    return 0;
}

static int cm_decl_primitive(uint8_t primitive, CmHirType *type)
{
    memset(type, 0, sizeof(*type));
    switch (primitive) {
    case CM_HIR_DECL_PRIMITIVE_UNIT:
        type->kind = CM_HIR_TYPE_UNIT_KIND; return 1;
    case CM_HIR_DECL_PRIMITIVE_BOOL:
        type->kind = CM_HIR_TYPE_BOOL_KIND; return 1;
    case CM_HIR_DECL_PRIMITIVE_CHAR:
        type->kind = CM_HIR_TYPE_CHAR_KIND; return 1;
    case CM_HIR_DECL_PRIMITIVE_STR:
        type->kind = CM_HIR_TYPE_STR_KIND; return 1;
    case CM_HIR_DECL_PRIMITIVE_I8:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_I8; return 1;
    case CM_HIR_DECL_PRIMITIVE_I16:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_I16; return 1;
    case CM_HIR_DECL_PRIMITIVE_I32:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_I32; return 1;
    case CM_HIR_DECL_PRIMITIVE_I64:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_I64; return 1;
    case CM_HIR_DECL_PRIMITIVE_I128:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_I128; return 1;
    case CM_HIR_DECL_PRIMITIVE_ISIZE:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_ISIZE; return 1;
    case CM_HIR_DECL_ENUM_REPR_U8:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_U8; return 1;
    case CM_HIR_DECL_ENUM_REPR_U16:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_U16; return 1;
    case CM_HIR_DECL_ENUM_REPR_U32:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_U32; return 1;
    case CM_HIR_DECL_ENUM_REPR_U64:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_U64; return 1;
    case CM_HIR_DECL_PRIMITIVE_U128:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_U128; return 1;
    case CM_HIR_DECL_PRIMITIVE_USIZE:
        type->kind = CM_HIR_TYPE_INTEGER_KIND;
        type->data.integer_type.kind = CM_HIR_INT_USIZE; return 1;
    case CM_HIR_DECL_PRIMITIVE_F32:
        type->kind = CM_HIR_TYPE_FLOAT_KIND;
        type->data.float_type.kind = CM_HIR_FLOAT_F32; return 1;
    case CM_HIR_DECL_PRIMITIVE_F64:
        type->kind = CM_HIR_TYPE_FLOAT_KIND;
        type->data.float_type.kind = CM_HIR_FLOAT_F64; return 1;
    }
    return 0;
}

static int cm_decl_library_primitive(uint32_t primitive,
    CmHirPrimitiveKind *out)
{
    if (out == NULL) return 0;
    switch (primitive) {
    case CM_HIR_DECL_PRIMITIVE_BOOL:
        *out = CM_HIR_PRIMITIVE_BOOL; return 1;
    case CM_HIR_DECL_PRIMITIVE_CHAR:
        *out = CM_HIR_PRIMITIVE_CHAR; return 1;
    case CM_HIR_DECL_PRIMITIVE_STR:
        *out = CM_HIR_PRIMITIVE_STR; return 1;
    case CM_HIR_DECL_PRIMITIVE_I8:
        *out = CM_HIR_PRIMITIVE_I8; return 1;
    case CM_HIR_DECL_PRIMITIVE_I16:
        *out = CM_HIR_PRIMITIVE_I16; return 1;
    case CM_HIR_DECL_PRIMITIVE_I32:
        *out = CM_HIR_PRIMITIVE_I32; return 1;
    case CM_HIR_DECL_PRIMITIVE_I64:
        *out = CM_HIR_PRIMITIVE_I64; return 1;
    case CM_HIR_DECL_PRIMITIVE_I128:
        *out = CM_HIR_PRIMITIVE_I128; return 1;
    case CM_HIR_DECL_PRIMITIVE_ISIZE:
        *out = CM_HIR_PRIMITIVE_ISIZE; return 1;
    case CM_HIR_DECL_PRIMITIVE_U8:
        *out = CM_HIR_PRIMITIVE_U8; return 1;
    case CM_HIR_DECL_PRIMITIVE_U16:
        *out = CM_HIR_PRIMITIVE_U16; return 1;
    case CM_HIR_DECL_PRIMITIVE_U32:
        *out = CM_HIR_PRIMITIVE_U32; return 1;
    case CM_HIR_DECL_PRIMITIVE_U64:
        *out = CM_HIR_PRIMITIVE_U64; return 1;
    case CM_HIR_DECL_PRIMITIVE_U128:
        *out = CM_HIR_PRIMITIVE_U128; return 1;
    case CM_HIR_DECL_PRIMITIVE_USIZE:
        *out = CM_HIR_PRIMITIVE_USIZE; return 1;
    case CM_HIR_DECL_PRIMITIVE_F32:
        *out = CM_HIR_PRIMITIVE_F32; return 1;
    case CM_HIR_DECL_PRIMITIVE_F64:
        *out = CM_HIR_PRIMITIVE_F64; return 1;
    default:
        return 0;
    }
}

static CmHirStatus cm_decl_reserve(CmHirContext *context,
    CmHirCrateId crate_id, const CmHirDeclarationMetadata *metadata,
    CmDeclRuntime *runtime, CmSpan span)
{
    size_t index;
    CmHirStatus status;
    for (index = 0u; index < metadata->trait_count; ++index) {
        status = cm_hir_reserve_item_definition_as(context, crate_id,
            CM_HIR_ITEM_TRAIT, span, &runtime->traits[index]);
        if (status != CM_HIR_OK) return status;
    }
    for (index = 0u; index < metadata->associated_count; ++index) {
        if (metadata->associated_items[index].kind
                != CM_HIR_DECL_ASSOCIATED_METHOD) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        status = cm_hir_reserve_item_definition_as(context, crate_id,
            CM_HIR_ITEM_FUNCTION, span, &runtime->associated[index]);
        if (status != CM_HIR_OK) return status;
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        CmHirItemKind kind;
        if (metadata->items[index].kind == CM_HIR_DECL_ITEM_STRUCT) {
            kind = CM_HIR_ITEM_STRUCT;
        } else if (metadata->items[index].kind
                == CM_HIR_DECL_ITEM_UNION) {
            kind = CM_HIR_ITEM_UNION;
        } else if (metadata->items[index].kind
                == CM_HIR_DECL_ITEM_ENUM) {
            kind = CM_HIR_ITEM_ENUM;
        } else if (metadata->items[index].kind
                == CM_HIR_DECL_ITEM_TYPE_ALIAS) {
            kind = CM_HIR_ITEM_TYPE_ALIAS;
        } else {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        status = cm_hir_reserve_item_definition_as(context, crate_id,
            kind, span, &runtime->items[index]);
        if (status != CM_HIR_OK) return status;
        if (kind == CM_HIR_ITEM_ENUM) {
            uint32_t variant_index;

            for (variant_index = 0u;
                    variant_index < metadata->items[index].variant_count;
                    ++variant_index) {
                status = cm_hir_reserve_enum_variant_definition(context,
                    crate_id, cm_decl_span(span.source,
                        metadata->items[index].variants[variant_index]
                            .source_ordinal),
                    &runtime->variants[index][variant_index]);
                if (status != CM_HIR_OK) return status;
            }
        }
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        CmHirItemKind kind;
        if (metadata->values[index].kind == CM_HIR_DECL_VALUE_FUNCTION) {
            kind = CM_HIR_ITEM_FUNCTION;
        } else if (metadata->values[index].kind
                == CM_HIR_DECL_VALUE_CONST) {
            kind = CM_HIR_ITEM_CONST;
        } else if (metadata->values[index].kind
                == CM_HIR_DECL_VALUE_STATIC) {
            kind = CM_HIR_ITEM_STATIC;
        } else {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        status = cm_hir_reserve_item_definition_as(context, crate_id,
            kind, span, &runtime->values[index]);
        if (status != CM_HIR_OK) return status;
    }
    return CM_HIR_OK;
}

static CmHirStatus cm_decl_add_generics(CmHirContext *context,
    const CmHirDeclarationMetadata *metadata, CmDeclRuntime *runtime,
    CmSourceId source)
{
    size_t index;
    for (index = 0u; index < metadata->generic_count; ++index) {
        const CmHirDeclarationGeneric *wire = &metadata->generics[index];
        CmHirGenericParam parameter;
        CmHirStatus status;
        memset(&parameter, 0, sizeof(parameter));
        parameter.kind = CM_HIR_GENERIC_TYPE;
        if (wire->owner_kind == CM_HIR_DECL_GENERIC_NOMINAL
            && wire->owner_local != 0u
            && (size_t)wire->owner_local <= metadata->trait_count) {
            parameter.owner = runtime->traits[wire->owner_local - 1u];
        } else if (wire->owner_kind == CM_HIR_DECL_GENERIC_ITEM
            && wire->owner_local != 0u
            && (size_t)wire->owner_local <= metadata->item_count) {
            parameter.owner = runtime->items[wire->owner_local - 1u];
        } else if (wire->owner_kind == CM_HIR_DECL_GENERIC_VALUE
            && wire->owner_local != 0u
            && (size_t)wire->owner_local <= metadata->value_count) {
            parameter.owner = runtime->values[wire->owner_local - 1u];
        } else {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        parameter.index = wire->index;
        parameter.name = cm_decl_intern(context, wire->name);
        parameter.span = cm_decl_span(source, (uint32_t)(index + 1u));
        parameter.is_relaxed_sized = wire->is_relaxed_sized;
        status = cm_hir_add_generic_param(context, &parameter,
            &runtime->generics[index]);
        if (status != CM_HIR_OK) return status;
    }
    return CM_HIR_OK;
}

static int cm_decl_mutability(uint8_t wire, CmHirMutability *out)
{
    if (wire == CM_HIR_DECL_IMMUTABLE) {
        *out = CM_HIR_IMMUTABLE;
        return 1;
    }
    if (wire == CM_HIR_DECL_MUTABLE) {
        *out = CM_HIR_MUTABLE;
        return 1;
    }
    return 0;
}

static int cm_decl_visibility(CmHirDeclarationVisibility wire,
    CmHirVisibility *out)
{
    memset(out, 0, sizeof(*out));
    out->restriction = cm_hir_def_id_none();
    if (wire.kind == CM_HIR_DECL_VISIBILITY_PRIVATE
        && wire.restriction_module == 0u) {
        out->kind = CM_HIR_VIS_PRIVATE;
        return 1;
    }
    if (wire.kind == CM_HIR_DECL_VISIBILITY_PUBLIC
        && wire.restriction_module == 0u) {
        out->kind = CM_HIR_VIS_PUBLIC;
        return 1;
    }
    if (wire.kind == CM_HIR_DECL_VISIBILITY_CRATE
        && wire.restriction_module == 0u) {
        out->kind = CM_HIR_VIS_CRATE;
        return 1;
    }
    return 0;
}

static CmInternId cm_decl_lang_attribute(CmHirContext *context,
    CmHirDeclarationString lang_item)
{
    static const unsigned char prefix[] = "lang = \"";
    static const unsigned char suffix[] = "\"";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t suffix_length = sizeof(suffix) - 1u;
    size_t length;
    unsigned char *metadata;
    CmInternId result;
    if (lang_item.data == NULL || lang_item.length == 0u
        || lang_item.length > SIZE_MAX - prefix_length
        || prefix_length + lang_item.length > SIZE_MAX - suffix_length) {
        return CM_INTERN_ID_NONE;
    }
    length = prefix_length + lang_item.length + suffix_length;
    metadata = (unsigned char *)cm_alloc(length);
    if (metadata == NULL) return CM_INTERN_ID_NONE;
    memcpy(metadata, prefix, prefix_length);
    memcpy(metadata + prefix_length, lang_item.data, lang_item.length);
    memcpy(metadata + prefix_length + lang_item.length, suffix,
        suffix_length);
    result = cm_interner_intern(&context->strings, metadata, length);
    cm_free(metadata);
    return result;
}

static CmHirStatus cm_decl_aggregate_attributes(CmHirContext *context,
    const CmHirDeclarationItem *wire, CmSpan span,
    CmHirAttribute attributes[3], uint32_t *out_count)
{
    uint16_t known_flags = CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
        | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT;
    uint32_t count = 0u;
    if (context == NULL || wire == NULL || attributes == NULL
        || out_count == NULL
        || (wire->aggregate_flags & (uint16_t)~known_flags) != 0u
        || (wire->aggregate_repr != CM_HIR_DECL_AGGREGATE_REPR_RUST
            && wire->aggregate_repr
                != CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT)
        || ((wire->aggregate_flags
                & CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT) != 0u
            && wire->aggregate_repr
                != CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT)) {
        return CM_HIR_INVARIANT_VIOLATION;
    }
    memset(attributes, 0, 3u * sizeof(*attributes));
    if ((wire->aggregate_flags
            & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u) {
        attributes[count].metadata = cm_decl_lang_attribute(context,
            wire->lang_item);
        if (attributes[count].metadata == CM_INTERN_ID_NONE)
            return CM_HIR_INVALID_ARGUMENT;
        count += 1u;
    }
    if (wire->aggregate_repr
            == CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT) {
        attributes[count].metadata = cm_hir_intern(context,
            "repr(transparent)");
        if (attributes[count].metadata == CM_INTERN_ID_NONE)
            return CM_HIR_INVALID_ARGUMENT;
        count += 1u;
    }
    if ((wire->aggregate_flags
            & CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT) != 0u) {
        attributes[count].metadata = cm_hir_intern(context,
            "rustc_pub_transparent");
        if (attributes[count].metadata == CM_INTERN_ID_NONE)
            return CM_HIR_INVALID_ARGUMENT;
        count += 1u;
    }
    {
        uint32_t index;
        for (index = 0u; index < count; ++index) {
            attributes[index].span = span;
            /* Synthetic metadata-origin ordinals are always nonzero. */
            attributes[index].source_attribute = index + 1u;
        }
    }
    *out_count = count;
    return CM_HIR_OK;
}

static int cm_decl_explicit_enum_repr(uint8_t primitive,
    uint64_t *out_maximum, const char **out_attribute)
{
    if (out_maximum == NULL || out_attribute == NULL) return 0;
    switch (primitive) {
    case CM_HIR_DECL_PRIMITIVE_U8:
        *out_maximum = UINT8_MAX;
        *out_attribute = "repr(u8)";
        return 1;
    case CM_HIR_DECL_PRIMITIVE_U16:
        *out_maximum = UINT16_MAX;
        *out_attribute = "repr(u16)";
        return 1;
    case CM_HIR_DECL_PRIMITIVE_U32:
        *out_maximum = UINT32_MAX;
        *out_attribute = "repr(u32)";
        return 1;
    case CM_HIR_DECL_PRIMITIVE_U64:
        *out_maximum = UINT64_MAX;
        *out_attribute = "repr(u64)";
        return 1;
    default:
        return 0;
    }
}

static int cm_decl_application_schema_valid(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationType *wire)
{
    const CmHirDeclarationItem *target;
    uint32_t index;
    if (wire->item_local == 0u
        || (size_t)wire->item_local > metadata->item_count
        || wire->argument_count == 0u || wire->argument_types == NULL) {
        return 0;
    }
    target = &metadata->items[wire->item_local - 1u];
    if ((target->kind != CM_HIR_DECL_ITEM_STRUCT
            && target->kind != CM_HIR_DECL_ITEM_UNION
            && target->kind != CM_HIR_DECL_ITEM_ENUM)
        || target->generic_count != wire->argument_count
        || target->generic_start == 0u
        || (size_t)target->generic_start > metadata->generic_count
        || (size_t)target->generic_count
            > metadata->generic_count - (size_t)target->generic_start + 1u) {
        return 0;
    }
    for (index = 0u; index < target->generic_count; ++index) {
        const CmHirDeclarationGeneric *generic = &metadata->generics[
            target->generic_start - 1u + index];
        if (generic->owner_kind != CM_HIR_DECL_GENERIC_ITEM
            || generic->owner_local != wire->item_local
            || generic->index != index
            || generic->kind != CM_HIR_DECL_GENERIC_TYPE) return 0;
    }
    return 1;
}

static CmHirStatus cm_decl_add_types(CmHirContext *context,
    const CmHirDeclarationMetadata *metadata, CmDeclRuntime *runtime,
    CmSourceId source)
{
    size_t index;
    for (index = 0u; index < metadata->type_count; ++index) {
        const CmHirDeclarationType *wire = &metadata->types[index];
        CmHirGenericArg *arguments = NULL;
        CmHirTypeId *elements = NULL;
        CmHirType type;
        CmHirStatus status;
        if (wire->kind == CM_HIR_DECL_TYPE_PRIMITIVE) {
            if (!cm_decl_primitive(wire->primitive, &type))
                return CM_HIR_INVARIANT_VIOLATION;
        } else if (wire->kind == CM_HIR_DECL_TYPE_GENERIC) {
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_PARAMETER_KIND;
            type.data.parameter_type.parameter =
                runtime->generics[wire->generic_local - 1u];
        } else if (wire->kind == CM_HIR_DECL_TYPE_NAMED_ADT) {
            if (wire->item_local == 0u
                || (size_t)wire->item_local > metadata->item_count
                || (metadata->items[wire->item_local - 1u].kind
                        != CM_HIR_DECL_ITEM_STRUCT
                    && metadata->items[wire->item_local - 1u].kind
                        != CM_HIR_DECL_ITEM_UNION
                    && metadata->items[wire->item_local - 1u].kind
                        != CM_HIR_DECL_ITEM_ENUM)
                || metadata->items[wire->item_local - 1u].generic_count
                    != 0u) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_ADT_KIND;
            type.data.named_type.definition =
                runtime->items[wire->item_local - 1u];
        } else if (wire->kind == CM_HIR_DECL_TYPE_SLICE) {
            if (wire->child_type == 0u
                || (size_t)wire->child_type > index
                || runtime->types[wire->child_type - 1u]
                    == CM_HIR_TYPE_NONE) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_SLICE_KIND;
            type.data.slice_type.element =
                runtime->types[wire->child_type - 1u];
        } else if (wire->kind == CM_HIR_DECL_TYPE_RAW_POINTER) {
            if (wire->child_type == 0u
                || (size_t)wire->child_type > index
                || runtime->types[wire->child_type - 1u]
                    == CM_HIR_TYPE_NONE) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_RAW_POINTER_KIND;
            type.data.raw_pointer_type.pointee =
                runtime->types[wire->child_type - 1u];
            if (!cm_decl_mutability(wire->mutability,
                    &type.data.raw_pointer_type.mutability)) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
        } else if (wire->kind == CM_HIR_DECL_TYPE_SELF) {
            if (wire->self_trait_local == 0u
                || (size_t)wire->self_trait_local > metadata->trait_count
                || cm_hir_def_id_is_none(
                    runtime->traits[wire->self_trait_local - 1u])) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_SELF_KIND;
            type.data.self_type.owner =
                runtime->traits[wire->self_trait_local - 1u];
        } else if (wire->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            if (wire->child_type == 0u
                || (size_t)wire->child_type > index
                || runtime->types[wire->child_type - 1u]
                    == CM_HIR_TYPE_NONE
                || wire->region.generic_local != 0u
                || wire->region.binder_index != 0u
                || (wire->region.kind != CM_HIR_DECL_REGION_STATIC
                    && wire->region.kind != CM_HIR_DECL_REGION_ERASED)) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_REFERENCE_KIND;
            type.data.reference_type.pointee =
                runtime->types[wire->child_type - 1u];
            type.data.reference_type.region.kind = wire->region.kind
                    == CM_HIR_DECL_REGION_STATIC
                ? CM_HIR_REGION_STATIC : CM_HIR_REGION_ERASED;
            if (!cm_decl_mutability(wire->mutability,
                    &type.data.reference_type.mutability)) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
        } else if (wire->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            uint32_t child;
            if (!cm_decl_application_schema_valid(metadata, wire)) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            arguments = (CmHirGenericArg *)cm_decl_array(
                wire->argument_count, sizeof(*arguments));
            if (arguments == NULL) return CM_HIR_INVALID_ARGUMENT;
            for (child = 0u; child < wire->argument_count; ++child) {
                uint32_t argument = wire->argument_types[child];
                if (argument == 0u || (size_t)argument > index
                    || runtime->types[argument - 1u] == CM_HIR_TYPE_NONE) {
                    cm_free(arguments);
                    return CM_HIR_INVARIANT_VIOLATION;
                }
                arguments[child].kind = CM_HIR_GENERIC_ARG_TYPE;
                arguments[child].data.type = runtime->types[argument - 1u];
            }
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_ADT_KIND;
            type.data.named_type.definition =
                runtime->items[wire->item_local - 1u];
            type.data.named_type.arguments = arguments;
            type.data.named_type.argument_count = wire->argument_count;
        } else if (wire->kind == CM_HIR_DECL_TYPE_TUPLE) {
            uint32_t child;
            if (wire->element_count == 0u || wire->element_types == NULL) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            elements = (CmHirTypeId *)cm_decl_array(wire->element_count,
                sizeof(*elements));
            if (elements == NULL) return CM_HIR_INVALID_ARGUMENT;
            for (child = 0u; child < wire->element_count; ++child) {
                uint32_t element = wire->element_types[child];
                if (element == 0u || (size_t)element > index
                    || runtime->types[element - 1u] == CM_HIR_TYPE_NONE) {
                    cm_free(elements);
                    return CM_HIR_INVARIANT_VIOLATION;
                }
                elements[child] = runtime->types[element - 1u];
            }
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_TUPLE_KIND;
            type.data.tuple_type.elements = elements;
            type.data.tuple_type.element_count = wire->element_count;
        } else if (wire->kind == CM_HIR_DECL_TYPE_ARRAY) {
            const CmHirDeclarationType *length_type;
            if (wire->child_type == 0u
                || (size_t)wire->child_type > index
                || runtime->types[wire->child_type - 1u]
                    == CM_HIR_TYPE_NONE
                || wire->array_length_type == 0u
                || (size_t)wire->array_length_type > index
                || runtime->types[wire->array_length_type - 1u]
                    == CM_HIR_TYPE_NONE) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            length_type = &metadata->types[wire->array_length_type - 1u];
            if (length_type->kind != CM_HIR_DECL_TYPE_PRIMITIVE
                || length_type->primitive != CM_HIR_DECL_PRIMITIVE_USIZE
                || wire->array_length_high_bits != 0u) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            memset(&type, 0, sizeof(type));
            type.kind = CM_HIR_TYPE_ARRAY_KIND;
            type.data.array_type.element =
                runtime->types[wire->child_type - 1u];
            type.data.array_type.length.kind = CM_HIR_CONST_VALUE;
            type.data.array_type.length.type =
                runtime->types[wire->array_length_type - 1u];
            type.data.array_type.length.data.value.low_bits =
                wire->array_length_low_bits;
            type.data.array_type.length.data.value.high_bits =
                wire->array_length_high_bits;
        } else {
            /* Non-structural future tags fail closed here. */
            return CM_HIR_INVARIANT_VIOLATION;
        }
        type.span = cm_decl_span(source, (uint32_t)(index + 1u));
        status = cm_hir_add_type(context, &type, &runtime->types[index]);
        cm_free(elements);
        cm_free(arguments);
        if (status != CM_HIR_OK) return status;
    }
    return CM_HIR_OK;
}

static void cm_decl_free_predicate_arguments(CmHirGenericArg **arguments,
    size_t count);

static CmHirStatus cm_decl_bind_traits(CmHirContext *context,
    const CmHirDeclarationMetadata *metadata, const CmDeclRuntime *runtime,
    CmSourceId source)
{
    size_t index;
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirDeclarationTrait *wire = &metadata->traits[index];
        CmHirAttribute diagnostic_attribute;
        CmHirOutlivesPredicate *outlives = NULL;
        CmHirItem item;
        CmHirItemId item_id;
        CmHirStatus status;
        uint32_t predicate_index;
        memset(&item, 0, sizeof(item));
        memset(&diagnostic_attribute, 0, sizeof(diagnostic_attribute));
        item.kind = CM_HIR_ITEM_TRAIT;
        item.definition = runtime->traits[index];
        item.owner_module = runtime->modules[wire->owner_module - 1u];
        item.parent_definition = cm_hir_def_id_none();
        item.name = cm_decl_intern(context, wire->name);
        item.visibility.kind = CM_HIR_VIS_PUBLIC;
        item.visibility.restriction = cm_hir_def_id_none();
        item.span = cm_decl_span(source, wire->source_ordinal);
        item.generic_parameter_start = wire->generic_count == 0u
            ? CM_HIR_GENERIC_PARAM_NONE
            : runtime->generics[wire->generic_start - 1u];
        item.generic_parameter_count = wire->generic_count;
        if (wire->predicate_scope_start != 0u
            || wire->predicate_scope_count != 0u
            || wire->predicate_start != 0u || wire->predicate_count != 0u
            || (wire->flags
                & (uint8_t)~CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM) != 0u) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        if ((wire->flags & CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM) != 0u) {
            static const unsigned char prefix[] =
                "rustc_diagnostic_item = \"";
            static const unsigned char suffix[] = "\"";
            size_t prefix_length = sizeof(prefix) - 1u;
            size_t suffix_length = sizeof(suffix) - 1u;
            size_t total;
            unsigned char *text;
            if (wire->diagnostic_item.data == NULL
                || wire->diagnostic_item.length == 0u
                || wire->diagnostic_item.length
                    > SIZE_MAX - prefix_length - suffix_length) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            total = prefix_length + wire->diagnostic_item.length
                + suffix_length;
            text = (unsigned char *)cm_alloc(total);
            memcpy(text, prefix, prefix_length);
            memcpy(text + prefix_length, wire->diagnostic_item.data,
                wire->diagnostic_item.length);
            memcpy(text + prefix_length + wire->diagnostic_item.length,
                suffix, suffix_length);
            diagnostic_attribute.metadata = cm_interner_intern(
                &context->strings, text, total);
            cm_free(text);
            if (diagnostic_attribute.metadata == CM_INTERN_ID_NONE)
                return CM_HIR_INVALID_ARGUMENT;
            diagnostic_attribute.span = item.span;
            diagnostic_attribute.source_attribute = 1u;
            diagnostic_attribute.expansion_depth = 0u;
            item.attributes = &diagnostic_attribute;
            item.attribute_count = 1u;
        } else if (wire->diagnostic_item.data != NULL
                || wire->diagnostic_item.length != 0u) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        outlives = (CmHirOutlivesPredicate *)cm_decl_array(
            wire->outlives_count, sizeof(*outlives));
        if (wire->outlives_count != 0u && outlives == NULL)
            return CM_HIR_INVALID_ARGUMENT;
        for (predicate_index = 0u;
                predicate_index < wire->outlives_count;
                ++predicate_index) {
            const CmHirDeclarationOutlivesPredicate *predicate;
            uint32_t local;
            if (wire->outlives_start == 0u
                || (size_t)wire->outlives_start
                    > metadata->outlives_predicate_count
                || (size_t)predicate_index
                    >= metadata->outlives_predicate_count
                        - (size_t)wire->outlives_start + 1u) {
                cm_free(outlives);
                return CM_HIR_INVARIANT_VIOLATION;
            }
            predicate = &metadata->outlives_predicates[
                wire->outlives_start - 1u + predicate_index];
            local = predicate->subject_type;
            if (predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
                || predicate->owner_local != (uint32_t)(index + 1u)
                || predicate->ordinal != predicate_index
                || predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE
                || local == 0u || (size_t)local > metadata->type_count
                || runtime->types[local - 1u] == CM_HIR_TYPE_NONE
                || predicate->bound.kind != CM_HIR_DECL_REGION_STATIC
                || predicate->bound.generic_local != 0u
                || predicate->bound.binder_index != 0u) {
                cm_free(outlives);
                return CM_HIR_INVARIANT_VIOLATION;
            }
            outlives[predicate_index].subject_kind = CM_HIR_OUTLIVES_TYPE;
            outlives[predicate_index].subject.type =
                runtime->types[local - 1u];
            outlives[predicate_index].bound.kind = CM_HIR_REGION_STATIC;
            outlives[predicate_index].scope = CM_HIR_PREDICATE_SCOPE_NONE;
            outlives[predicate_index].span = item.span;
        }
        item.outlives_predicates = outlives;
        item.outlives_predicate_count = wire->outlives_count;
        if (wire->safety == CM_HIR_DECL_SAFETY_SAFE) {
            item.data.trait_item.safety = CM_HIR_SAFE;
        } else if (wire->safety == CM_HIR_DECL_SAFETY_UNSAFE) {
            item.data.trait_item.safety = CM_HIR_UNSAFE;
        } else {
            cm_free(outlives);
            return CM_HIR_INVARIANT_VIOLATION;
        }
        status = cm_hir_add_item(context, &item, &item_id);
        cm_free(outlives);
        if (status != CM_HIR_OK) return status;
    }
    return CM_HIR_OK;
}

static int cm_decl_receiver(uint8_t wire, CmHirReceiverKind *out)
{
    if (out == NULL) return 0;
    switch (wire) {
    case CM_HIR_DECL_RECEIVER_NONE:
        *out = CM_HIR_RECEIVER_NONE; return 1;
    case CM_HIR_DECL_RECEIVER_VALUE:
        *out = CM_HIR_RECEIVER_VALUE; return 1;
    case CM_HIR_DECL_RECEIVER_REF_SHARED:
        *out = CM_HIR_RECEIVER_REF_SHARED; return 1;
    case CM_HIR_DECL_RECEIVER_REF_MUTABLE:
        *out = CM_HIR_RECEIVER_REF_MUTABLE; return 1;
    case CM_HIR_DECL_RECEIVER_CUSTOM:
        *out = CM_HIR_RECEIVER_CUSTOM; return 1;
    default:
        return 0;
    }
}

static int cm_decl_safety(uint8_t wire, CmHirSafety *out)
{
    if (out == NULL) return 0;
    if (wire == CM_HIR_DECL_SAFETY_SAFE) {
        *out = CM_HIR_SAFE;
        return 1;
    }
    if (wire == CM_HIR_DECL_SAFETY_UNSAFE) {
        *out = CM_HIR_UNSAFE;
        return 1;
    }
    return 0;
}

static CmHirStatus cm_decl_bind_associated(CmHirContext *context,
    const CmHirDeclarationMetadata *metadata, const CmDeclRuntime *runtime,
    CmSourceId source)
{
    size_t associated_index;

    for (associated_index = 0u;
            associated_index < metadata->associated_count;
            ++associated_index) {
        const CmHirDeclarationAssociatedItem *wire =
            &metadata->associated_items[associated_index];
        const CmHirDeclarationTrait *parent_wire;
        CmHirFunctionParameter *parameters = NULL;
        CmHirTraitPredicate *predicates = NULL;
        CmHirGenericArg **arguments = NULL;
        CmHirItem item;
        CmHirItemId item_id;
        CmHirStatus status = CM_HIR_OK;
        CmHirReceiverKind receiver;
        CmHirSafety safety;
        size_t index;

        if (wire->kind != CM_HIR_DECL_ASSOCIATED_METHOD
            || wire->parent_kind != CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL
            || wire->parent_local == 0u
            || (size_t)wire->parent_local > metadata->trait_count
            || wire->implemented_associated_local != 0u
            || wire->parameter_count == 0u
            || wire->parameter_types == NULL
            || wire->return_type == 0u
            || (size_t)wire->return_type > metadata->type_count
            || wire->generic_count != 0u || wire->generic_start != 0u
            || wire->is_const != 0u || wire->is_async != 0u
            || wire->is_variadic != 0u
            || (wire->has_default_body != 0u
                && wire->has_default_body != 1u)
            || !cm_decl_receiver(wire->receiver, &receiver)
            || !cm_decl_safety(wire->safety, &safety)) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        parent_wire = &metadata->traits[wire->parent_local - 1u];
        parameters = (CmHirFunctionParameter *)cm_decl_array(
            wire->parameter_count, sizeof(*parameters));
        predicates = (CmHirTraitPredicate *)cm_decl_array(
            wire->predicate_count, sizeof(*predicates));
        arguments = (CmHirGenericArg **)cm_decl_array(
            wire->predicate_count, sizeof(*arguments));
        if (parameters == NULL || (wire->predicate_count != 0u
                && (predicates == NULL || arguments == NULL))) {
            status = CM_HIR_INVALID_ARGUMENT;
            goto done;
        }
        for (index = 0u; index < wire->parameter_count; ++index) {
            uint32_t type_local = wire->parameter_types[index];
            if (type_local == 0u
                || (size_t)type_local > metadata->type_count
                || runtime->types[type_local - 1u] == CM_HIR_TYPE_NONE) {
                status = CM_HIR_INVARIANT_VIOLATION;
                goto done;
            }
            parameters[index].name = index == 0u
                ? cm_hir_intern(context, "self") : CM_INTERN_ID_NONE;
            parameters[index].type = runtime->types[type_local - 1u];
            parameters[index].span = cm_decl_span(source,
                wire->source_ordinal);
            parameters[index].binding_kind = index == 0u
                ? CM_HIR_BINDING_NAMED : CM_HIR_BINDING_DISCARD;
            parameters[index].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
        }
        for (index = 0u; index < wire->predicate_count; ++index) {
            const CmHirDeclarationPredicate *predicate;
            uint32_t child;
            predicate = &metadata->predicates[
                wire->predicate_start - 1u + index];
            if (predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
                || predicate->owner_associated != associated_index + 1u
                || predicate->owner_value != 0u
                || predicate->subject_type == 0u
                || (size_t)predicate->subject_type > metadata->type_count
                || predicate->trait_local == 0u
                || (size_t)predicate->trait_local > metadata->trait_count) {
                status = CM_HIR_INVARIANT_VIOLATION;
                goto done;
            }
            arguments[index] = (CmHirGenericArg *)cm_decl_array(
                predicate->argument_count, sizeof(*arguments[index]));
            if (predicate->argument_count != 0u
                && arguments[index] == NULL) {
                status = CM_HIR_INVALID_ARGUMENT;
                goto done;
            }
            for (child = 0u; child < predicate->argument_count; ++child) {
                uint32_t argument = predicate->argument_types[child];
                if (argument == 0u
                    || (size_t)argument > metadata->type_count) {
                    status = CM_HIR_INVARIANT_VIOLATION;
                    goto done;
                }
                arguments[index][child].kind = CM_HIR_GENERIC_ARG_TYPE;
                arguments[index][child].data.type =
                    runtime->types[argument - 1u];
            }
            predicates[index].subject =
                runtime->types[predicate->subject_type - 1u];
            predicates[index].trait_type.definition =
                runtime->traits[predicate->trait_local - 1u];
            predicates[index].trait_type.arguments = arguments[index];
            predicates[index].trait_type.argument_count =
                predicate->argument_count;
            predicates[index].scope = CM_HIR_PREDICATE_SCOPE_NONE;
            predicates[index].span = cm_decl_span(source,
                wire->source_ordinal);
            predicates[index].modifier = CM_HIR_PREDICATE_REQUIRED;
        }
        memset(&item, 0, sizeof(item));
        item.kind = CM_HIR_ITEM_FUNCTION;
        item.definition = runtime->associated[associated_index];
        item.owner_module = runtime->modules[parent_wire->owner_module - 1u];
        item.parent_definition = runtime->traits[wire->parent_local - 1u];
        item.is_specializable = wire->is_specializable;
        item.name = cm_decl_intern(context, wire->name);
        if (!cm_decl_visibility(wire->visibility, &item.visibility)) {
            status = CM_HIR_INVARIANT_VIOLATION;
            goto done;
        }
        item.span = cm_decl_span(source, wire->source_ordinal);
        item.generic_parameter_start = CM_HIR_GENERIC_PARAM_NONE;
        item.predicates = predicates;
        item.predicate_count = wire->predicate_count;
        item.data.function_item.signature.parameters = parameters;
        item.data.function_item.signature.parameter_count =
            wire->parameter_count;
        item.data.function_item.signature.receiver = receiver;
        item.data.function_item.signature.return_type =
            runtime->types[wire->return_type - 1u];
        item.data.function_item.signature.abi =
            cm_decl_intern(context, wire->abi);
        item.data.function_item.signature.safety = safety;
        item.data.function_item.signature.is_const = wire->is_const;
        item.data.function_item.signature.is_async = wire->is_async;
        item.data.function_item.signature.is_variadic = wire->is_variadic;
        item.data.function_item.body = CM_HIR_BODY_NONE;
        item.data.function_item.has_default_body = wire->has_default_body;
        item.data.function_item.trait_item_definition = cm_hir_def_id_none();
        status = cm_hir_add_item(context, &item, &item_id);
done:
        cm_decl_free_predicate_arguments(arguments, wire->predicate_count);
        cm_free(predicates);
        cm_free(parameters);
        if (status != CM_HIR_OK) return status;
    }
    return CM_HIR_OK;
}

static CmHirStatus cm_decl_bind_items(CmHirContext *context,
    const CmHirDeclarationMetadata *metadata, const CmDeclRuntime *runtime,
    CmSourceId source)
{
    size_t index;
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *wire;
        CmHirAttribute enum_attributes[2];
        CmHirAttribute aggregate_attributes[3];
        CmHirField *fields;
        CmHirVariant *variants;
        unsigned char *diagnostic_metadata;
        CmHirItem item;
        CmHirItemId item_id;
        CmHirStatus status;
        wire = &metadata->items[index];
        if ((wire->kind != CM_HIR_DECL_ITEM_STRUCT
                && wire->kind != CM_HIR_DECL_ITEM_UNION
                && wire->kind != CM_HIR_DECL_ITEM_ENUM
                && wire->kind != CM_HIR_DECL_ITEM_TYPE_ALIAS)
            || (wire->visibility.kind != CM_HIR_DECL_VISIBILITY_PRIVATE
                && wire->visibility.kind
                    != CM_HIR_DECL_VISIBILITY_PUBLIC)
            || wire->visibility.restriction_module != 0u) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        fields = NULL;
        variants = NULL;
        diagnostic_metadata = NULL;
        memset(enum_attributes, 0, sizeof(enum_attributes));
        memset(aggregate_attributes, 0, sizeof(aggregate_attributes));
        memset(&item, 0, sizeof(item));
        if (wire->kind == CM_HIR_DECL_ITEM_STRUCT) {
            item.kind = CM_HIR_ITEM_STRUCT;
        } else if (wire->kind == CM_HIR_DECL_ITEM_UNION) {
            item.kind = CM_HIR_ITEM_UNION;
        } else if (wire->kind == CM_HIR_DECL_ITEM_ENUM) {
            item.kind = CM_HIR_ITEM_ENUM;
        } else {
            item.kind = CM_HIR_ITEM_TYPE_ALIAS;
        }
        item.definition = runtime->items[index];
        item.owner_module = runtime->modules[wire->owner_module - 1u];
        item.parent_definition = cm_hir_def_id_none();
        item.name = cm_decl_intern(context, wire->name);
        if (!cm_decl_visibility(wire->visibility, &item.visibility))
            return CM_HIR_INVARIANT_VIOLATION;
        item.span = cm_decl_span(source, wire->source_ordinal);
        item.generic_parameter_start = wire->generic_count == 0u
            ? CM_HIR_GENERIC_PARAM_NONE
            : runtime->generics[wire->generic_start - 1u];
        item.generic_parameter_count = wire->generic_count;
        if (wire->kind == CM_HIR_DECL_ITEM_STRUCT
                || wire->kind == CM_HIR_DECL_ITEM_UNION) {
            uint32_t field_index;
            uint32_t attribute_count;
            if ((wire->aggregate_form != CM_HIR_DECL_AGGREGATE_UNIT
                    && wire->aggregate_form
                        != CM_HIR_DECL_AGGREGATE_TUPLE
                    && wire->aggregate_form
                        != CM_HIR_DECL_AGGREGATE_NAMED)
                || (wire->kind == CM_HIR_DECL_ITEM_UNION
                    && wire->aggregate_form
                        != CM_HIR_DECL_AGGREGATE_NAMED)
                || (wire->aggregate_form == CM_HIR_DECL_AGGREGATE_UNIT
                    && (wire->field_count != 0u || wire->fields != NULL))
                || (wire->aggregate_form != CM_HIR_DECL_AGGREGATE_UNIT
                    && (wire->field_count == 0u || wire->fields == NULL))) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            item.data.aggregate_item.form =
                wire->aggregate_form == CM_HIR_DECL_AGGREGATE_UNIT
                    ? CM_HIR_AGGREGATE_UNIT
                    : wire->aggregate_form == CM_HIR_DECL_AGGREGATE_TUPLE
                    ? CM_HIR_AGGREGATE_TUPLE : CM_HIR_AGGREGATE_NAMED;
            fields = (CmHirField *)cm_decl_array(wire->field_count,
                sizeof(*fields));
            if (wire->field_count != 0u && fields == NULL)
                return CM_HIR_INVALID_ARGUMENT;
            for (field_index = 0u; field_index < wire->field_count;
                    ++field_index) {
                const CmHirDeclarationField *field =
                    &wire->fields[field_index];
                if (field->type_local == 0u
                    || (size_t)field->type_local > metadata->type_count
                    || runtime->types[field->type_local - 1u]
                        == CM_HIR_TYPE_NONE
                    || (wire->aggregate_form
                            == CM_HIR_DECL_AGGREGATE_TUPLE
                        && (field->name.data != NULL
                            || field->name.length != 0u))
                    || !cm_decl_visibility(field->visibility,
                        &fields[field_index].visibility)) {
                    cm_free(fields);
                    return CM_HIR_INVARIANT_VIOLATION;
                }
                fields[field_index].name = wire->aggregate_form
                        == CM_HIR_DECL_AGGREGATE_TUPLE
                    ? CM_INTERN_ID_NONE : cm_decl_intern(context,
                        field->name);
                fields[field_index].type =
                    runtime->types[field->type_local - 1u];
                fields[field_index].span = cm_decl_span(source,
                    field->source_ordinal);
            }
            status = cm_decl_aggregate_attributes(context, wire, item.span,
                aggregate_attributes, &attribute_count);
            if (status != CM_HIR_OK) {
                cm_free(fields);
                return status;
            }
            item.attributes = aggregate_attributes;
            item.attribute_count = attribute_count;
            item.data.aggregate_item.fields = fields;
            item.data.aggregate_item.field_count = wire->field_count;
        } else if (wire->kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t variant_index;

            uint64_t maximum_discriminant = 0u;
            const char *repr_attribute = NULL;
            int explicit_discriminants = cm_decl_explicit_enum_repr(
                wire->enum_repr_primitive, &maximum_discriminant,
                &repr_attribute);
            if (wire->alias_target_type != 0u
                || (wire->enum_flags
                    & (uint8_t)~CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u
                || (!explicit_discriminants
                    && wire->enum_repr_primitive
                        != CM_HIR_DECL_ENUM_REPR_RUST)
                || wire->variant_count == 0u || wire->variants == NULL
                || runtime->variants[index] == NULL) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            if (explicit_discriminants) {
                if (wire->diagnostic_item.data != NULL
                    || wire->diagnostic_item.length != 0u
                    || wire->generic_count != 0u
                    || wire->enum_flags != 0u) {
                    return CM_HIR_INVARIANT_VIOLATION;
                }
            } else {
                static const unsigned char prefix[] =
                    "rustc_diagnostic_item = \"";
                static const unsigned char suffix[] = "\"";
                size_t prefix_length = sizeof(prefix) - 1u;
                size_t suffix_length = sizeof(suffix) - 1u;
                size_t metadata_length;

                if (wire->diagnostic_item.data == NULL
                    || wire->diagnostic_item.length == 0u
                    || wire->diagnostic_item.length
                        > SIZE_MAX - prefix_length
                    || prefix_length + wire->diagnostic_item.length
                        > SIZE_MAX - suffix_length) {
                    return CM_HIR_INVARIANT_VIOLATION;
                }
                metadata_length = prefix_length
                    + wire->diagnostic_item.length + suffix_length;
                diagnostic_metadata = (unsigned char *)cm_alloc(
                    metadata_length);
                memcpy(diagnostic_metadata, prefix, prefix_length);
                memcpy(diagnostic_metadata + prefix_length,
                    wire->diagnostic_item.data,
                    wire->diagnostic_item.length);
                memcpy(diagnostic_metadata + prefix_length
                        + wire->diagnostic_item.length,
                    suffix, suffix_length);
                enum_attributes[0].metadata = cm_interner_intern(
                    &context->strings, diagnostic_metadata,
                    metadata_length);
                cm_free(diagnostic_metadata);
                diagnostic_metadata = NULL;
                if (enum_attributes[0].metadata == CM_INTERN_ID_NONE)
                    return CM_HIR_INVALID_ARGUMENT;
                if ((wire->enum_flags
                        & CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u) {
                    enum_attributes[1].metadata = cm_decl_lang_attribute(
                        context, wire->enum_lang_item);
                    if (enum_attributes[1].metadata == CM_INTERN_ID_NONE)
                        return CM_HIR_INVALID_ARGUMENT;
                } else if (wire->enum_lang_item.data != NULL
                        || wire->enum_lang_item.length != 0u) {
                    return CM_HIR_INVARIANT_VIOLATION;
                }
            }
            variants = (CmHirVariant *)cm_decl_array(wire->variant_count,
                sizeof(*variants));
            if (variants == NULL) return CM_HIR_INVALID_ARGUMENT;
            for (variant_index = 0u; variant_index < wire->variant_count;
                    ++variant_index) {
                const CmHirDeclarationVariant *wire_variant;
                CmHirType discriminant_type;
                CmHirTypeId discriminant_type_id;
                uint32_t field_index;

                wire_variant = &wire->variants[variant_index];
                if ((wire_variant->kind != CM_HIR_DECL_VARIANT_UNIT
                        && wire_variant->kind
                            != CM_HIR_DECL_VARIANT_TUPLE)
                    || (wire_variant->kind == CM_HIR_DECL_VARIANT_UNIT
                        && (wire_variant->field_count != 0u
                            || wire_variant->fields != NULL))
                    || (wire_variant->kind == CM_HIR_DECL_VARIANT_TUPLE
                        && (explicit_discriminants
                            || wire_variant->field_count == 0u
                            || wire_variant->fields == NULL))
                    || (explicit_discriminants
                        ? (wire_variant->discriminant_primitive
                                != CM_HIR_DECL_PRIMITIVE_ISIZE
                            || wire_variant->discriminant_high != 0u
                            || wire_variant->discriminant_low
                                > maximum_discriminant)
                        : (wire_variant->discriminant_primitive
                                != CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT
                            || wire_variant->discriminant_low != 0u
                            || wire_variant->discriminant_high != 0u))
                    || (wire_variant->flags
                        & (uint16_t)~CM_HIR_DECL_VARIANT_HAS_LANG_ITEM)
                        != 0u) {
                    status = CM_HIR_INVARIANT_VIOLATION;
                    goto enum_failure;
                }
                variants[variant_index].definition =
                    runtime->variants[index][variant_index];
                variants[variant_index].name = cm_decl_intern(context,
                    wire_variant->name);
                if ((wire_variant->flags
                        & CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) != 0u) {
                    variants[variant_index].lang_item = cm_decl_intern(
                        context, wire_variant->lang_item);
                    if (variants[variant_index].lang_item
                            == CM_INTERN_ID_NONE) {
                        status = CM_HIR_INVALID_ARGUMENT;
                        goto enum_failure;
                    }
                } else if (wire_variant->lang_item.data != NULL
                        || wire_variant->lang_item.length != 0u) {
                    status = CM_HIR_INVARIANT_VIOLATION;
                    goto enum_failure;
                }
                variants[variant_index].form = wire_variant->kind
                        == CM_HIR_DECL_VARIANT_UNIT
                    ? CM_HIR_AGGREGATE_UNIT : CM_HIR_AGGREGATE_TUPLE;
                variants[variant_index].field_count =
                    wire_variant->field_count;
                variants[variant_index].fields = (CmHirField *)cm_decl_array(
                    wire_variant->field_count,
                    sizeof(*variants[variant_index].fields));
                if (wire_variant->field_count != 0u
                    && variants[variant_index].fields == NULL) {
                    status = CM_HIR_INVALID_ARGUMENT;
                    goto enum_failure;
                }
                for (field_index = 0u;
                     field_index < wire_variant->field_count;
                     ++field_index) {
                    const CmHirDeclarationVariantField *wire_field;

                    wire_field = &wire_variant->fields[field_index];
                    if (wire_field->type_local == 0u
                        || (size_t)wire_field->type_local
                            > metadata->type_count
                        || runtime->types[wire_field->type_local - 1u]
                            == CM_HIR_TYPE_NONE) {
                        status = CM_HIR_INVARIANT_VIOLATION;
                        goto enum_failure;
                    }
                    variants[variant_index].fields[field_index].name =
                        CM_INTERN_ID_NONE;
                    variants[variant_index].fields[field_index].type =
                        runtime->types[wire_field->type_local - 1u];
                    variants[variant_index].fields[field_index].visibility
                        .kind = CM_HIR_VIS_PRIVATE;
                    variants[variant_index].fields[field_index].visibility
                        .restriction = cm_hir_def_id_none();
                    variants[variant_index].fields[field_index].span =
                        cm_decl_span(source, wire_field->source_ordinal);
                }
                if (explicit_discriminants) {
                    memset(&discriminant_type, 0,
                        sizeof(discriminant_type));
                    discriminant_type.kind = CM_HIR_TYPE_INTEGER_KIND;
                    discriminant_type.data.integer_type.kind =
                        CM_HIR_INT_ISIZE;
                    discriminant_type.span = cm_decl_span(source,
                        wire_variant->source_ordinal);
                    status = cm_hir_add_type(context, &discriminant_type,
                        &discriminant_type_id);
                    if (status != CM_HIR_OK) {
                        goto enum_failure;
                    }
                    variants[variant_index].has_discriminant = 1;
                    variants[variant_index].discriminant.kind =
                        CM_HIR_CONST_VALUE;
                    variants[variant_index].discriminant.type =
                        discriminant_type_id;
                    variants[variant_index].discriminant.data.value.low_bits =
                        wire_variant->discriminant_low;
                    variants[variant_index].discriminant.data.value.high_bits =
                        wire_variant->discriminant_high;
                }
                variants[variant_index].span = cm_decl_span(source,
                    wire_variant->source_ordinal);
            }
            if (explicit_discriminants) {
                enum_attributes[0].metadata = cm_hir_intern(context,
                    repr_attribute);
                if (enum_attributes[0].metadata == CM_INTERN_ID_NONE) {
                    status = CM_HIR_INVALID_ARGUMENT;
                    goto enum_failure;
                }
            }
            {
                uint32_t attribute_count;
                uint32_t attribute_index;

                attribute_count = explicit_discriminants ? 1u
                    : (((wire->enum_flags
                            & CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u)
                        ? 2u : 1u);
                for (attribute_index = 0u;
                     attribute_index < attribute_count; ++attribute_index) {
                    enum_attributes[attribute_index].span = item.span;
                    enum_attributes[attribute_index].source_attribute =
                        attribute_index + 1u;
                }
                item.attributes = enum_attributes;
                item.attribute_count = attribute_count;
            }
            item.data.enum_item.variants = variants;
            item.data.enum_item.variant_count = wire->variant_count;
        } else {
            if (wire->alias_target_type == 0u
                || (size_t)wire->alias_target_type > metadata->type_count) {
                return CM_HIR_INVARIANT_VIOLATION;
            }
            item.data.type_alias_item.target =
                runtime->types[wire->alias_target_type - 1u];
            item.data.type_alias_item.trait_item_definition =
                cm_hir_def_id_none();
        }
        status = cm_hir_add_item(context, &item, &item_id);
        cm_free(fields);
        if (variants != NULL) {
            uint32_t variant_index;
            for (variant_index = 0u;
                 variant_index < wire->variant_count; ++variant_index) {
                cm_free(variants[variant_index].fields);
            }
        }
        cm_free(variants);
        if (status != CM_HIR_OK) return status;
        continue;

enum_failure:
        if (variants != NULL) {
            uint32_t cleanup_index;
            for (cleanup_index = 0u;
                 cleanup_index < wire->variant_count; ++cleanup_index) {
                cm_free(variants[cleanup_index].fields);
            }
        }
        cm_free(variants);
        cm_free(fields);
        return status;
    }
    return CM_HIR_OK;
}

static void cm_decl_free_predicate_arguments(CmHirGenericArg **arguments,
    size_t count)
{
    size_t index;
    if (arguments == NULL) return;
    for (index = 0u; index < count; ++index) cm_free(arguments[index]);
    cm_free(arguments);
}

static CmHirStatus cm_decl_bind_value(CmHirContext *context,
    const CmHirDeclarationMetadata *metadata, const CmDeclRuntime *runtime,
    size_t value_index, CmSourceId source)
{
    const CmHirDeclarationValue *wire = &metadata->values[value_index];
    CmHirFunctionParameter *parameters;
    CmHirTraitPredicate *predicates;
    CmHirGenericArg **arguments;
    CmHirItem item;
    CmHirItemId item_id;
    CmHirStatus status;
    size_t index;
    if (wire->kind == CM_HIR_DECL_VALUE_CONST
            || wire->kind == CM_HIR_DECL_VALUE_STATIC) {
        CmHirMutability mutability;
        if (!cm_decl_mutability(wire->mutability, &mutability)
            || (wire->kind == CM_HIR_DECL_VALUE_CONST
                && mutability != CM_HIR_IMMUTABLE)
            || wire->declared_type == 0u
            || (size_t)wire->declared_type > metadata->type_count
            || runtime->types[wire->declared_type - 1u]
                == CM_HIR_TYPE_NONE
            || wire->generic_start != 0u || wire->generic_count != 0u
            || wire->predicate_start != 0u || wire->predicate_count != 0u
            || wire->parameter_count != 0u
            || wire->parameter_types != NULL || wire->return_type != 0u
            || wire->has_body != 1u) {
            return CM_HIR_INVARIANT_VIOLATION;
        }
        memset(&item, 0, sizeof(item));
        item.kind = wire->kind == CM_HIR_DECL_VALUE_CONST
            ? CM_HIR_ITEM_CONST : CM_HIR_ITEM_STATIC;
        item.definition = runtime->values[value_index];
        item.owner_module = runtime->modules[wire->owner_module - 1u];
        item.parent_definition = cm_hir_def_id_none();
        item.name = cm_decl_intern(context, wire->name);
        item.visibility.kind = CM_HIR_VIS_PUBLIC;
        item.visibility.restriction = cm_hir_def_id_none();
        item.span = cm_decl_span(source, wire->source_ordinal);
        item.data.value_item.type = runtime->types[wire->declared_type - 1u];
        item.data.value_item.body = CM_HIR_BODY_NONE;
        item.data.value_item.definition_kind =
            CM_HIR_VALUE_DEFINITION_METADATA_DECLARATION;
        item.data.value_item.has_default_body = 0;
        item.data.value_item.mutability = mutability;
        item.data.value_item.trait_item_definition = cm_hir_def_id_none();
        return cm_hir_add_item(context, &item, &item_id);
    }
    if (wire->kind != CM_HIR_DECL_VALUE_FUNCTION)
        return CM_HIR_INVARIANT_VIOLATION;
    parameters = (CmHirFunctionParameter *)cm_decl_array(
        wire->parameter_count, sizeof(*parameters));
    predicates = (CmHirTraitPredicate *)cm_decl_array(wire->predicate_count,
        sizeof(*predicates));
    arguments = (CmHirGenericArg **)cm_decl_array(wire->predicate_count,
        sizeof(*arguments));
    if ((wire->parameter_count != 0u && parameters == NULL)
        || (wire->predicate_count != 0u
            && (predicates == NULL || arguments == NULL))) {
        status = CM_HIR_INVALID_ARGUMENT;
        goto done;
    }
    for (index = 0u; index < wire->parameter_count; ++index) {
        parameters[index].name = CM_INTERN_ID_NONE;
        parameters[index].type = runtime->types[
            wire->parameter_types[index] - 1u];
        parameters[index].span = cm_decl_span(source, wire->source_ordinal);
        parameters[index].binding_kind = CM_HIR_BINDING_DISCARD;
        parameters[index].binding_mode = CM_HIR_PARAMETER_BINDING_MOVE;
    }
    for (index = 0u; index < wire->predicate_count; ++index) {
        const CmHirDeclarationPredicate *predicate = &metadata->predicates[
            wire->predicate_start - 1u + index];
        uint32_t child;
        arguments[index] = (CmHirGenericArg *)cm_decl_array(
            predicate->argument_count, sizeof(*arguments[index]));
        if (predicate->argument_count != 0u && arguments[index] == NULL) {
            status = CM_HIR_INVALID_ARGUMENT;
            goto done;
        }
        for (child = 0u; child < predicate->argument_count; ++child) {
            arguments[index][child].kind = CM_HIR_GENERIC_ARG_TYPE;
            arguments[index][child].data.type = runtime->types[
                predicate->argument_types[child] - 1u];
        }
        predicates[index].subject = runtime->types[
            predicate->subject_type - 1u];
        predicates[index].trait_type.definition = runtime->traits[
            predicate->trait_local - 1u];
        predicates[index].trait_type.arguments = arguments[index];
        predicates[index].trait_type.argument_count = predicate->argument_count;
        predicates[index].scope = CM_HIR_PREDICATE_SCOPE_NONE;
        predicates[index].span = cm_decl_span(source, wire->source_ordinal);
        predicates[index].modifier = CM_HIR_PREDICATE_REQUIRED;
    }
    memset(&item, 0, sizeof(item));
    item.kind = CM_HIR_ITEM_FUNCTION;
    item.definition = runtime->values[value_index];
    item.owner_module = runtime->modules[wire->owner_module - 1u];
    item.parent_definition = cm_hir_def_id_none();
    item.name = cm_decl_intern(context, wire->name);
    item.visibility.kind = CM_HIR_VIS_PUBLIC;
    item.visibility.restriction = cm_hir_def_id_none();
    item.span = cm_decl_span(source, wire->source_ordinal);
    item.generic_parameter_start = runtime->generics[wire->generic_start - 1u];
    item.generic_parameter_count = wire->generic_count;
    item.predicates = predicates;
    item.predicate_count = wire->predicate_count;
    item.data.function_item.signature.parameters = parameters;
    item.data.function_item.signature.parameter_count = wire->parameter_count;
    item.data.function_item.signature.receiver = CM_HIR_RECEIVER_NONE;
    item.data.function_item.signature.return_type =
        runtime->types[wire->return_type - 1u];
    item.data.function_item.signature.abi = cm_hir_intern(context, "Rust");
    item.data.function_item.signature.safety = CM_HIR_SAFE;
    item.data.function_item.body = CM_HIR_BODY_NONE;
    status = cm_hir_add_item(context, &item, &item_id);
done:
    cm_decl_free_predicate_arguments(arguments, wire->predicate_count);
    cm_free(predicates);
    cm_free(parameters);
    return status;
}

static CmHirLibraryStatus cm_decl_add_library_value(CmHirContext *context,
    CmHirLibraryOwnedData *owned, const CmHirDeclarationMetadata *metadata,
    const CmDeclRuntime *runtime, size_t value_index)
{
    const CmHirDeclarationValue *wire = &metadata->values[value_index];
    const CmHirDefinition *definition;
    const CmHirItem *item;
    CmHirLibraryNominalReference *references;
    CmHirGenericParamKind **schemas;
    CmHirLibraryValue value;
    uint32_t reference_count;
    size_t trait_index;
    CmHirLibraryStatus status;
    definition = cm_hir_lookup_definition(context, runtime->values[value_index]);
    item = definition == NULL ? NULL
        : cm_hir_get_item(context, definition->entity.item_id);
    if (definition == NULL || item == NULL) return CM_HIR_LIBRARY_INVALID_HIR;
    if (wire->kind == CM_HIR_DECL_VALUE_CONST
            || wire->kind == CM_HIR_DECL_VALUE_STATIC) {
        CmHirItemKind expected_item_kind =
            wire->kind == CM_HIR_DECL_VALUE_CONST
                ? CM_HIR_ITEM_CONST : CM_HIR_ITEM_STATIC;
        CmHirLibraryValueKind expected_value_kind =
            wire->kind == CM_HIR_DECL_VALUE_CONST
                ? CM_HIR_LIBRARY_VALUE_CONST : CM_HIR_LIBRARY_VALUE_STATIC;
        if (item->kind != expected_item_kind
            || item->data.value_item.definition_kind
                != CM_HIR_VALUE_DEFINITION_METADATA_DECLARATION
            || item->data.value_item.body != CM_HIR_BODY_NONE
            || (wire->kind == CM_HIR_DECL_VALUE_CONST
                && item->data.value_item.mutability != CM_HIR_IMMUTABLE)) {
            return CM_HIR_LIBRARY_INVALID_HIR;
        }
        memset(&value, 0, sizeof(value));
        value.definition = item->definition;
        value.kind = expected_value_kind;
        value.data.value.type = item->data.value_item.type;
        value.data.value.mutability = item->data.value_item.mutability;
        return cm_hir_library_owned_data_add_value(owned, &value);
    }
    if (wire->kind != CM_HIR_DECL_VALUE_FUNCTION
        || item->kind != CM_HIR_ITEM_FUNCTION)
        return CM_HIR_LIBRARY_INVALID_HIR;
    references = (CmHirLibraryNominalReference *)cm_decl_array(
        metadata->trait_count, sizeof(*references));
    schemas = (CmHirGenericParamKind **)cm_decl_array(metadata->trait_count,
        sizeof(*schemas));
    if ((metadata->trait_count != 0u
            && (references == NULL || schemas == NULL))) {
        status = CM_HIR_LIBRARY_INVALID_ARGUMENT;
        goto done;
    }
    reference_count = 0u;
    for (trait_index = 0u; trait_index < metadata->trait_count; ++trait_index) {
        const CmHirDeclarationTrait *trait_wire;
        const CmHirModule *owner;
        uint32_t predicate_index;
        int referenced;
        referenced = 0;
        for (predicate_index = 0u; predicate_index < wire->predicate_count;
                ++predicate_index) {
            const CmHirDeclarationPredicate *predicate;
            predicate = &metadata->predicates[
                wire->predicate_start - 1u + predicate_index];
            if (predicate->trait_local == trait_index + 1u) referenced = 1;
        }
        if (!referenced) continue;
        trait_wire = &metadata->traits[trait_index];
        owner = cm_hir_get_module(context,
            runtime->modules[trait_wire->owner_module - 1u]);
        if (owner == NULL) {
            status = CM_HIR_LIBRARY_INVALID_HIR;
            goto done;
        }
        schemas[reference_count] = (CmHirGenericParamKind *)cm_decl_array(
            trait_wire->generic_count, sizeof(*schemas[reference_count]));
        if (trait_wire->generic_count != 0u
            && schemas[reference_count] == NULL) {
            status = CM_HIR_LIBRARY_INVALID_ARGUMENT;
            goto done;
        }
        for (predicate_index = 0u;
                predicate_index < trait_wire->generic_count;
                ++predicate_index) {
            schemas[reference_count][predicate_index] = CM_HIR_GENERIC_TYPE;
        }
        references[reference_count].definition = runtime->traits[trait_index];
        references[reference_count].owner_module = owner->definition;
        references[reference_count].name.bytes = trait_wire->name.data;
        references[reference_count].name.length = trait_wire->name.length;
        references[reference_count].use = CM_HIR_LIBRARY_REFERENCE_ONLY;
        references[reference_count].kind = CM_HIR_LIBRARY_NOMINAL_TRAIT;
        references[reference_count].declaring_trait = cm_hir_def_id_none();
        references[reference_count].generic_parameter_kinds =
            schemas[reference_count];
        references[reference_count].generic_parameter_count =
            trait_wire->generic_count;
        reference_count += 1u;
    }
    memset(&value, 0, sizeof(value));
    value.definition = item->definition;
    value.kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    /* Function parameter structs are not a TypeId array. Build the exact list. */
    {
        CmHirTypeId *parameter_types;
        uint32_t index;
        parameter_types = (CmHirTypeId *)cm_decl_array(wire->parameter_count,
            sizeof(*parameter_types));
        if (wire->parameter_count != 0u && parameter_types == NULL) {
            status = CM_HIR_LIBRARY_INVALID_ARGUMENT;
            goto done;
        }
        for (index = 0u; index < wire->parameter_count; ++index)
            parameter_types[index] =
                item->data.function_item.signature.parameters[index].type;
        value.data.function.parameter_types = parameter_types;
        value.data.function.parameter_count = wire->parameter_count;
        value.data.function.return_type =
            item->data.function_item.signature.return_type;
        value.data.function.generic_parameter_start =
            item->generic_parameter_start;
        value.data.function.generic_parameter_count =
            item->generic_parameter_count;
        value.data.function.predicates = item->predicates;
        value.data.function.predicate_count = item->predicate_count;
        value.data.function.nominal_references = references;
        value.data.function.nominal_reference_count = reference_count;
        value.data.function.abi = item->data.function_item.signature.abi;
        value.data.function.safety = item->data.function_item.signature.safety;
        status = cm_hir_library_owned_data_add_value(owned, &value);
        cm_free(parameter_types);
    }
done:
    if (schemas != NULL) {
        for (trait_index = 0u; trait_index < metadata->trait_count;
                ++trait_index) cm_free(schemas[trait_index]);
    }
    cm_free(schemas);
    cm_free(references);
    return status;
}

static CmHirLibraryStatus cm_decl_add_library_associated(
    CmHirContext *context, CmHirLibraryOwnedData *owned,
    const CmHirDeclarationMetadata *metadata,
    const CmDeclRuntime *runtime, size_t associated_index)
{
    const CmHirDeclarationAssociatedItem *wire =
        &metadata->associated_items[associated_index];
    const CmHirDefinition *definition;
    const CmHirItem *item;
    CmHirLibraryNominalReference *references = NULL;
    CmHirGenericParamKind **schemas = NULL;
    CmHirTypeId *parameter_types = NULL;
    CmHirLibraryValue value;
    uint32_t reference_count = 0u;
    size_t trait_index;
    CmHirLibraryStatus status = CM_HIR_LIBRARY_INVALID_HIR;

    definition = cm_hir_lookup_definition(context,
        runtime->associated[associated_index]);
    item = definition == NULL ? NULL
        : cm_hir_get_item(context, definition->entity.item_id);
    if (definition == NULL || item == NULL
        || wire->kind != CM_HIR_DECL_ASSOCIATED_METHOD
        || item->kind != CM_HIR_ITEM_FUNCTION
        || wire->parent_local == 0u
        || (size_t)wire->parent_local > metadata->trait_count
        || !cm_hir_def_id_equal(item->parent_definition,
            runtime->traits[wire->parent_local - 1u])) {
        return CM_HIR_LIBRARY_INVALID_HIR;
    }
    references = (CmHirLibraryNominalReference *)cm_decl_array(
        metadata->trait_count, sizeof(*references));
    schemas = (CmHirGenericParamKind **)cm_decl_array(metadata->trait_count,
        sizeof(*schemas));
    if (metadata->trait_count != 0u
        && (references == NULL || schemas == NULL)) {
        status = CM_HIR_LIBRARY_INVALID_ARGUMENT;
        goto done;
    }
    for (trait_index = 0u; trait_index < metadata->trait_count;
            ++trait_index) {
        const CmHirDeclarationTrait *trait_wire;
        const CmHirModule *owner;
        uint32_t predicate_index;
        int referenced = 0;

        for (predicate_index = 0u;
                predicate_index < wire->predicate_count;
                ++predicate_index) {
            const CmHirDeclarationPredicate *predicate =
                &metadata->predicates[
                    wire->predicate_start - 1u + predicate_index];
            if (predicate->trait_local == trait_index + 1u) referenced = 1;
        }
        if (!referenced) continue;
        trait_wire = &metadata->traits[trait_index];
        owner = cm_hir_get_module(context,
            runtime->modules[trait_wire->owner_module - 1u]);
        if (owner == NULL) goto done;
        schemas[reference_count] = (CmHirGenericParamKind *)cm_decl_array(
            trait_wire->generic_count, sizeof(*schemas[reference_count]));
        if (trait_wire->generic_count != 0u
            && schemas[reference_count] == NULL) {
            status = CM_HIR_LIBRARY_INVALID_ARGUMENT;
            goto done;
        }
        for (predicate_index = 0u;
                predicate_index < trait_wire->generic_count;
                ++predicate_index) {
            schemas[reference_count][predicate_index] = CM_HIR_GENERIC_TYPE;
        }
        references[reference_count].definition = runtime->traits[trait_index];
        references[reference_count].owner_module = owner->definition;
        references[reference_count].name.bytes = trait_wire->name.data;
        references[reference_count].name.length = trait_wire->name.length;
        references[reference_count].use = CM_HIR_LIBRARY_REFERENCE_ONLY;
        references[reference_count].kind = CM_HIR_LIBRARY_NOMINAL_TRAIT;
        references[reference_count].declaring_trait = cm_hir_def_id_none();
        references[reference_count].generic_parameter_kinds =
            schemas[reference_count];
        references[reference_count].generic_parameter_count =
            trait_wire->generic_count;
        reference_count += 1u;
    }
    parameter_types = (CmHirTypeId *)cm_decl_array(wire->parameter_count,
        sizeof(*parameter_types));
    if (wire->parameter_count != 0u && parameter_types == NULL) {
        status = CM_HIR_LIBRARY_INVALID_ARGUMENT;
        goto done;
    }
    for (trait_index = 0u; trait_index < wire->parameter_count;
            ++trait_index) {
        parameter_types[trait_index] =
            item->data.function_item.signature.parameters[trait_index].type;
    }
    memset(&value, 0, sizeof(value));
    value.definition = item->definition;
    value.kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
    value.data.function.parameter_types = parameter_types;
    value.data.function.parameter_count = wire->parameter_count;
    value.data.function.return_type =
        item->data.function_item.signature.return_type;
    value.data.function.generic_parameter_start =
        item->generic_parameter_start;
    value.data.function.generic_parameter_count =
        item->generic_parameter_count;
    value.data.function.predicates = item->predicates;
    value.data.function.predicate_count = item->predicate_count;
    value.data.function.nominal_references = reference_count == 0u
        ? NULL : references;
    value.data.function.nominal_reference_count = reference_count;
    value.data.function.abi = item->data.function_item.signature.abi;
    value.data.function.safety = item->data.function_item.signature.safety;
    value.data.function.is_const =
        item->data.function_item.signature.is_const;
    value.data.function.is_async =
        item->data.function_item.signature.is_async;
    value.data.function.is_variadic =
        item->data.function_item.signature.is_variadic;
    value.data.function.parent_trait = item->parent_definition;
    value.data.function.receiver =
        item->data.function_item.signature.receiver;
    value.data.function.has_default_body =
        item->data.function_item.has_default_body;
    status = cm_hir_library_owned_data_add_value(owned, &value);
done:
    cm_free(parameter_types);
    if (schemas != NULL) {
        for (trait_index = 0u; trait_index < metadata->trait_count;
                ++trait_index) cm_free(schemas[trait_index]);
    }
    cm_free(schemas);
    cm_free(references);
    return status;
}

static CmHirLibraryStatus cm_decl_build_owned(CmHirContext *context,
    CmHirLibraryOwnedData *owned, const CmHirDeclarationMetadata *metadata,
    const CmDeclRuntime *runtime)
{
    size_t index;
    for (index = 0u; index < metadata->module_count; ++index) {
        const CmHirModule *module = cm_hir_get_module(context,
            runtime->modules[index]);
        size_t added;
        CmHirLibraryStatus status;
        if (module == NULL) return CM_HIR_LIBRARY_INVALID_HIR;
        status = cm_hir_library_owned_data_add_module(owned,
            module->definition, &added);
        if (status != CM_HIR_LIBRARY_OK) return status;
        if (added != index) return CM_HIR_LIBRARY_INVALID_HIR;
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        CmHirLibraryStatus status = cm_decl_add_library_value(context, owned,
            metadata, runtime, index);
        if (status != CM_HIR_LIBRARY_OK) return status;
    }
    for (index = 0u; index < metadata->associated_count; ++index) {
        CmHirLibraryStatus status = cm_decl_add_library_associated(context,
            owned, metadata, runtime, index);
        if (status != CM_HIR_LIBRARY_OK) return status;
    }
    for (index = 0u; index < metadata->namespace_count; ++index) {
        const CmHirDeclarationNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        CmHirLibraryBinding binding;
        CmHirLibraryStatus status;
        memset(&binding, 0, sizeof(binding));
        binding.type_kind = CM_HIR_TYPE_ERROR_KIND;
        binding.primitive_kind = CM_HIR_PRIMITIVE_NONE;
        binding.value_kind = CM_HIR_LIBRARY_VALUE_NONE;
        if (entry->target_kind == CM_HIR_DECL_TARGET_MODULE) {
            const CmHirModule *target = cm_hir_get_module(context,
                runtime->modules[entry->target_local - 1u]);
            if (target == NULL) return CM_HIR_LIBRARY_INVALID_HIR;
            binding.kind = CM_HIR_LIBRARY_BINDING_MODULE;
            binding.definition = target->definition;
        } else if (entry->target_kind == CM_HIR_DECL_TARGET_NOMINAL) {
            binding.kind = CM_HIR_LIBRARY_BINDING_TRAIT;
            binding.definition = runtime->traits[entry->target_local - 1u];
        } else if (entry->target_kind == CM_HIR_DECL_TARGET_ITEM) {
            const CmHirDeclarationItem *item = &metadata->items[
                entry->target_local - 1u];
            binding.definition = runtime->items[entry->target_local - 1u];
            if (item->kind == CM_HIR_DECL_ITEM_STRUCT) {
                binding.type_kind = CM_HIR_TYPE_ADT_KIND;
                if (entry->namespace_kind
                        == CM_HIR_DECL_NAMESPACE_VALUE) {
                    if (item->aggregate_form
                            != CM_HIR_DECL_AGGREGATE_UNIT) {
                        return CM_HIR_LIBRARY_INVALID_HIR;
                    }
                    binding.kind =
                        CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR;
                } else {
                    binding.kind = CM_HIR_LIBRARY_BINDING_TYPE;
                }
            } else if (item->kind == CM_HIR_DECL_ITEM_UNION
                    && entry->namespace_kind
                        == CM_HIR_DECL_NAMESPACE_TYPE) {
                binding.kind = CM_HIR_LIBRARY_BINDING_TYPE;
                binding.type_kind = CM_HIR_TYPE_ADT_KIND;
            } else if (item->kind == CM_HIR_DECL_ITEM_ENUM
                    && entry->namespace_kind
                        == CM_HIR_DECL_NAMESPACE_TYPE) {
                binding.kind = CM_HIR_LIBRARY_BINDING_TYPE;
                binding.type_kind = CM_HIR_TYPE_ADT_KIND;
            } else if (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
                    && entry->namespace_kind
                        == CM_HIR_DECL_NAMESPACE_TYPE) {
                binding.kind = CM_HIR_LIBRARY_BINDING_TYPE;
                binding.type_kind = CM_HIR_TYPE_ALIAS_APPLICATION_KIND;
            } else {
                return CM_HIR_LIBRARY_INVALID_HIR;
            }
        } else if (entry->target_kind == CM_HIR_DECL_TARGET_VALUE) {
            const CmHirDeclarationValue *value = &metadata->values[
                entry->target_local - 1u];
            binding.kind = CM_HIR_LIBRARY_BINDING_VALUE;
            binding.definition = runtime->values[entry->target_local - 1u];
            if (value->kind == CM_HIR_DECL_VALUE_FUNCTION) {
                binding.value_kind = CM_HIR_LIBRARY_VALUE_FUNCTION;
            } else if (value->kind == CM_HIR_DECL_VALUE_CONST) {
                binding.value_kind = CM_HIR_LIBRARY_VALUE_CONST;
            } else if (value->kind == CM_HIR_DECL_VALUE_STATIC) {
                binding.value_kind = CM_HIR_LIBRARY_VALUE_STATIC;
            } else {
                return CM_HIR_LIBRARY_INVALID_HIR;
            }
        } else if (entry->target_kind
                == CM_HIR_DECL_TARGET_ENUM_VARIANT) {
            CmHirDefId enum_definition;
            CmHirDefId variant_definition;
            uint32_t variant_index;
            const CmHirDefinition *parent;
            const CmHirItem *enum_item;

            if (!cm_decl_runtime_variant(metadata, runtime,
                    entry->target_local, &enum_definition,
                    &variant_definition, &variant_index)) {
                return CM_HIR_LIBRARY_INVALID_HIR;
            }
            parent = cm_hir_lookup_definition(context, enum_definition);
            enum_item = parent == NULL ? NULL
                : cm_hir_get_item(context, parent->entity.item_id);
            if (parent == NULL || parent->kind != CM_HIR_DEFINITION_ITEM
                || parent->state != CM_HIR_DEFINITION_BOUND
                || enum_item == NULL || enum_item->kind != CM_HIR_ITEM_ENUM
                || variant_index >= enum_item->data.enum_item.variant_count
                || !cm_hir_def_id_equal(
                    enum_item->data.enum_item.variants[variant_index]
                        .definition,
                    variant_definition)) {
                return CM_HIR_LIBRARY_INVALID_HIR;
            }
            binding.kind = CM_HIR_LIBRARY_BINDING_ENUM_VARIANT;
            binding.definition = variant_definition;
            binding.type_kind = CM_HIR_TYPE_ADT_KIND;
            binding.enum_definition = enum_definition;
            binding.enum_variant_index = variant_index;
            binding.enum_variant_form =
                enum_item->data.enum_item.variants[variant_index].form;
            binding.enum_variant_namespace = entry->namespace_kind
                    == CM_HIR_DECL_NAMESPACE_VALUE
                ? CM_HIR_LIBRARY_ENUM_VARIANT_VALUE
                : CM_HIR_LIBRARY_ENUM_VARIANT_TYPE;
        } else if (entry->target_kind == CM_HIR_DECL_TARGET_PRIMITIVE) {
            if (entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
                || !cm_decl_library_primitive(entry->target_local,
                    &binding.primitive_kind)) {
                return CM_HIR_LIBRARY_INVALID_HIR;
            }
            binding.kind = CM_HIR_LIBRARY_BINDING_PRIMITIVE;
        } else {
            return CM_HIR_LIBRARY_INVALID_HIR;
        }
        status = cm_hir_library_owned_data_add_entry(owned,
            entry->owner_module - 1u, entry->name.data, entry->name.length,
            &binding);
        if (status != CM_HIR_LIBRARY_OK) return status;
    }
    return CM_HIR_LIBRARY_OK;
}

CmHirDeclarationMaterializeResult cm_hir_declaration_metadata_materialize(
    CmHirContext *context, CmHirLibraryArtifact *artifact,
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationMaterializeExpectation *expectation,
    const char *extern_name, CmSourceId metadata_source)
{
    CmHirDeclarationMaterializeResult result = cm_decl_result(
        CM_HIR_DECL_MATERIALIZE_INVALID_ARGUMENT);
    CmHirDeclarationMetadataStatus metadata_status;
    CmHirEdition edition;
    CmDeclRuntime runtime;
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
        || expectation == NULL
        || extern_name == NULL || extern_name[0] == '\0'
        || metadata_source == 0u) return result;
    metadata_status = cm_hir_declaration_metadata_validate(metadata);
    if (metadata_status != CM_HIR_DECL_METADATA_OK) {
        result.status = CM_HIR_DECL_MATERIALIZE_INVALID_METADATA;
        result.metadata_status = metadata_status;
        return result;
    }
    if (!cm_decl_expectation_matches(metadata, expectation)) {
        result.status = CM_HIR_DECL_MATERIALIZE_INVALID_METADATA;
        result.metadata_status = CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR;
        return result;
    }
    if (!cm_decl_edition(metadata->edition, &edition)
        || metadata->root_module == 0u
        || !cm_decl_string_equal(metadata->crate_name,
            metadata->modules[metadata->root_module - 1u].name)) {
        result.status = CM_HIR_DECL_MATERIALIZE_INVALID_METADATA;
        result.metadata_status = CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR;
        return result;
    }
    if (!cm_decl_runtime_init(&runtime, metadata)) {
        result.status = CM_HIR_DECL_MATERIALIZE_HIR_FAILURE;
        result.hir_status = CM_HIR_INVALID_ARGUMENT;
        goto cleanup;
    }
    result.hir_status = cm_hir_context_mark(context, &mark);
    if (result.hir_status != CM_HIR_OK) {
        result.status = CM_HIR_DECL_MATERIALIZE_HIR_FAILURE;
        goto cleanup;
    }
    mark_active = 1;
    span = cm_decl_span(metadata_source, 0u);
    result.hir_status = cm_hir_create_crate(context,
        cm_decl_intern(context, metadata->crate_name), edition, span,
        &crate_id, &root_module);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    runtime.modules[metadata->root_module - 1u] = root_module;
    for (index = 0u; index < metadata->module_count; ++index) {
        const CmHirDeclarationModule *module;
        if (index + 1u == metadata->root_module) continue;
        module = &metadata->modules[index];
        result.hir_status = cm_hir_add_module(context, crate_id,
            runtime.modules[module->parent_module - 1u],
            cm_decl_intern(context, module->name),
            cm_decl_span(metadata_source, (uint32_t)index),
            &runtime.modules[index]);
        if (result.hir_status != CM_HIR_OK) goto hir_failure;
    }
    result.hir_status = cm_decl_reserve(context, crate_id, metadata, &runtime,
        span);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    result.hir_status = cm_decl_add_generics(context, metadata, &runtime,
        metadata_source);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    result.hir_status = cm_decl_add_types(context, metadata, &runtime,
        metadata_source);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    result.hir_status = cm_decl_bind_traits(context, metadata, &runtime,
        metadata_source);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    result.hir_status = cm_decl_bind_associated(context, metadata, &runtime,
        metadata_source);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    result.hir_status = cm_decl_bind_items(context, metadata, &runtime,
        metadata_source);
    if (result.hir_status != CM_HIR_OK) goto hir_failure;
    for (index = 0u; index < metadata->value_count; ++index) {
        result.hir_status = cm_decl_bind_value(context, metadata, &runtime,
            index, metadata_source);
        if (result.hir_status != CM_HIR_OK) goto hir_failure;
    }
    cm_hir_library_owned_data_init(&owned);
    owned_active = 1;
    result.library_status = cm_decl_build_owned(context, &owned, metadata,
        &runtime);
    if (result.library_status != CM_HIR_LIBRARY_OK) goto artifact_failure;
    cm_hir_library_artifact_init(&candidate);
    candidate_active = 1;
    {
        const CmHirModule *root = cm_hir_get_module(context, root_module);
        CmHirLibraryArtifactResult restored;
        if (root == NULL) {
            result.library_status = CM_HIR_LIBRARY_INVALID_HIR;
            goto artifact_failure;
        }
        restored = cm_hir_library_artifact_restore_owned(&candidate, context,
            crate_id, root->definition, extern_name, &owned);
        result.library_status = restored.status;
        if (restored.status != CM_HIR_LIBRARY_OK) goto artifact_failure;
        result.module_count = restored.module_count;
        result.item_count = metadata->item_count;
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
    result.status = CM_HIR_DECL_MATERIALIZE_OK;
    result.crate_id = crate_id;
    result.root_module = root_module;
    goto cleanup;
hir_failure:
    result.status = CM_HIR_DECL_MATERIALIZE_HIR_FAILURE;
    goto rollback;
artifact_failure:
    result.status = CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE;
rollback:
    if (mark_active) {
        (void)cm_hir_context_rewind(context, &mark);
        mark_active = 0;
    }
cleanup:
    if (candidate_active) cm_hir_library_artifact_destroy(&candidate);
    if (owned_active) cm_hir_library_owned_data_destroy(&owned);
    cm_decl_runtime_destroy(&runtime);
    return result;
}

const char *cm_hir_declaration_materialize_status_name(
    CmHirDeclarationMaterializeStatus status)
{
    switch (status) {
    case CM_HIR_DECL_MATERIALIZE_OK: return "ok";
    case CM_HIR_DECL_MATERIALIZE_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_DECL_MATERIALIZE_INVALID_METADATA: return "invalid metadata";
    case CM_HIR_DECL_MATERIALIZE_HIR_FAILURE: return "HIR failure";
    case CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE: return "artifact failure";
    }
    return "unknown declaration materialization status";
}
