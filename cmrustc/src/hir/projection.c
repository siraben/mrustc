#include "cm/hir/projection.h"

#include "cm/hir/type_alias.h"

typedef struct CmProjectionKey {
    CmHirTypeKind kind;
    union {
        CmHirIntType integer_kind;
        CmHirFloatType float_kind;
        CmHirDefId definition;
    } data;
} CmProjectionKey;

typedef enum CmProjectionSelfClass {
    CM_PROJECTION_SELF_UNSUPPORTED = 0,
    CM_PROJECTION_SELF_FOREIGN,
    CM_PROJECTION_SELF_EXACT,
    CM_PROJECTION_SELF_GENERIC_ADT
} CmProjectionSelfClass;

typedef struct CmProjectionSelf {
    CmProjectionSelfClass class_kind;
    CmProjectionKey exact_key;
    CmHirDefId adt_definition;
    const CmHirGenericArg *arguments;
    uint32_t argument_count;
} CmProjectionSelf;

typedef enum CmProjectionCandidateMatch {
    CM_PROJECTION_CANDIDATE_UNEQUAL = 0,
    CM_PROJECTION_CANDIDATE_EQUAL,
    CM_PROJECTION_CANDIDATE_DEFERRED
} CmProjectionCandidateMatch;

static CmHirProjectionMatch cm_projection_match_result(
    CmHirProjectionStatus status)
{
    CmHirProjectionMatch result;

    result.status = status;
    result.target_template = CM_HIR_TYPE_NONE;
    result.query_self = CM_HIR_TYPE_NONE;
    result.impl_definition = cm_hir_def_id_none();
    result.impl_associated_definition = cm_hir_def_id_none();
    return result;
}

static CmHirProjectionResult cm_projection_result(
    CmHirProjectionStatus status)
{
    CmHirProjectionResult result;

    result.status = status;
    result.target = CM_HIR_TYPE_NONE;
    result.impl_definition = cm_hir_def_id_none();
    result.impl_associated_definition = cm_hir_def_id_none();
    result.hir_status = CM_HIR_OK;
    result.allocated_type_count = 0u;
    return result;
}

static CmHirProjectionImplTarget cm_projection_impl_target_result(
    CmHirProjectionStatus status)
{
    CmHirProjectionImplTarget result;

    result.status = status;
    result.target_template = CM_HIR_TYPE_NONE;
    result.impl_associated_definition = cm_hir_def_id_none();
    return result;
}

static const CmHirItem *cm_projection_bound_item(
    const CmHirContext *context, CmHirDefId definition_id)
{
    const CmHirDefinition *definition;
    const CmHirItem *item;

    definition = cm_hir_lookup_definition(context, definition_id);
    if (definition == NULL || definition->kind != CM_HIR_DEFINITION_ITEM
        || definition->state != CM_HIR_DEFINITION_BOUND) {
        return NULL;
    }
    item = cm_hir_get_item(context, definition->entity.item_id);
    if (item == NULL
        || !cm_hir_def_id_equal(item->definition, definition_id)) {
        return NULL;
    }
    return item;
}

static int cm_projection_scalar_key(const CmHirType *type,
    CmProjectionKey *out_key)
{
    out_key->kind = type->kind;
    switch (type->kind) {
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
        return 1;
    case CM_HIR_TYPE_INTEGER_KIND:
        out_key->data.integer_kind = type->data.integer_type.kind;
        return 1;
    case CM_HIR_TYPE_FLOAT_KIND:
        out_key->data.float_kind = type->data.float_type.kind;
        return 1;
    default:
        return 0;
    }
}

static int cm_projection_key_equal(const CmProjectionKey *left,
    const CmProjectionKey *right)
{
    if (left->kind != right->kind) return 0;
    switch (left->kind) {
    case CM_HIR_TYPE_BOOL_KIND:
    case CM_HIR_TYPE_CHAR_KIND:
        return 1;
    case CM_HIR_TYPE_INTEGER_KIND:
        return left->data.integer_kind == right->data.integer_kind;
    case CM_HIR_TYPE_FLOAT_KIND:
        return left->data.float_kind == right->data.float_kind;
    case CM_HIR_TYPE_ADT_KIND:
        return cm_hir_def_id_equal(left->data.definition,
            right->data.definition);
    default:
        return 0;
    }
}

static int cm_projection_type_parameter_range_is_type_only(
    const CmHirContext *context, const CmHirItem *item,
    int require_no_defaults)
{
    uint32_t index;

    if (item->generic_parameter_count == 0u) {
        return item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE;
    }
    if (item->generic_parameter_start == CM_HIR_GENERIC_PARAM_NONE) {
        return 0;
    }
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmHirGenericParam *parameter;

        parameter = cm_hir_get_generic_param(context,
            item->generic_parameter_start + index);
        if (parameter == NULL
            || !cm_hir_def_id_equal(parameter->owner, item->definition)
            || parameter->index != index
            || parameter->kind != CM_HIR_GENERIC_TYPE
            || (require_no_defaults && parameter->has_default)) {
            return 0;
        }
    }
    return 1;
}

static CmProjectionSelf cm_projection_query_self(
    const CmHirContext *context, const CmHirType *type,
    CmHirCrateId local_crate)
{
    CmProjectionSelf result;
    const CmHirItem *definition_item;
    uint32_t index;

    result.class_kind = CM_PROJECTION_SELF_UNSUPPORTED;
    result.adt_definition = cm_hir_def_id_none();
    result.arguments = NULL;
    result.argument_count = 0u;
    if (cm_projection_scalar_key(type, &result.exact_key)) {
        result.class_kind = CM_PROJECTION_SELF_EXACT;
        return result;
    }
    if (type->kind != CM_HIR_TYPE_ADT_KIND) {
        return result;
    }
    if (type->data.named_type.definition.crate_id != local_crate) {
        result.class_kind = CM_PROJECTION_SELF_FOREIGN;
        return result;
    }
    definition_item = cm_projection_bound_item(context,
        type->data.named_type.definition);
    if (definition_item == NULL
        || (definition_item->kind != CM_HIR_ITEM_STRUCT
            && definition_item->kind != CM_HIR_ITEM_ENUM)) {
        return result;
    }
    if (definition_item->generic_parameter_count == 0u) {
        if (type->data.named_type.argument_count != 0u
            || type->data.named_type.arguments != NULL) {
            return result;
        }
        result.class_kind = CM_PROJECTION_SELF_EXACT;
        result.exact_key.kind = CM_HIR_TYPE_ADT_KIND;
        result.exact_key.data.definition =
            type->data.named_type.definition;
        result.adt_definition = type->data.named_type.definition;
        return result;
    }
    if (!cm_projection_type_parameter_range_is_type_only(context,
            definition_item, 0)
        || type->data.named_type.argument_count
            != definition_item->generic_parameter_count
        || type->data.named_type.arguments == NULL) {
        return result;
    }
    for (index = 0u; index < type->data.named_type.argument_count; ++index) {
        const CmHirGenericArg *argument;

        argument = &type->data.named_type.arguments[index];
        if (argument->kind != CM_HIR_GENERIC_ARG_TYPE
            || cm_hir_get_type(context, argument->data.type) == NULL) {
            return result;
        }
    }
    result.class_kind = CM_PROJECTION_SELF_GENERIC_ADT;
    result.adt_definition = type->data.named_type.definition;
    result.arguments = type->data.named_type.arguments;
    result.argument_count = type->data.named_type.argument_count;
    return result;
}

static int cm_projection_exact_candidate_key(const CmHirContext *context,
    const CmHirItem *item, CmHirCrateId local_crate,
    CmProjectionKey *out_key)
{
    const CmHirType *type;
    const CmHirItem *definition_item;

    if (item->generic_parameter_count != 0u) return 0;
    type = cm_hir_get_type(context, item->data.impl_item.self_type);
    if (type == NULL) return 0;
    if (cm_projection_scalar_key(type, out_key)) return 1;
    if (type->kind != CM_HIR_TYPE_ADT_KIND
        || type->data.named_type.argument_count != 0u
        || type->data.named_type.arguments != NULL
        || type->data.named_type.definition.crate_id != local_crate) {
        return 0;
    }
    definition_item = cm_projection_bound_item(context,
        type->data.named_type.definition);
    if (definition_item == NULL
        || (definition_item->kind != CM_HIR_ITEM_STRUCT
            && definition_item->kind != CM_HIR_ITEM_ENUM)
        || definition_item->generic_parameter_count != 0u) {
        return 0;
    }
    out_key->kind = CM_HIR_TYPE_ADT_KIND;
    out_key->data.definition = type->data.named_type.definition;
    return 1;
}

static int cm_projection_ordered_generic_candidate(
    const CmHirContext *context, const CmHirItem *item,
    const CmProjectionSelf *query)
{
    const CmHirType *self_type;
    const CmHirItem *adt_item;
    uint32_t index;

    if (item->generic_parameter_count == 0u
        || query->class_kind != CM_PROJECTION_SELF_GENERIC_ADT
        || !cm_projection_type_parameter_range_is_type_only(context, item,
            1)) {
        return 0;
    }
    self_type = cm_hir_get_type(context, item->data.impl_item.self_type);
    if (self_type == NULL || self_type->kind != CM_HIR_TYPE_ADT_KIND
        || !cm_hir_def_id_equal(self_type->data.named_type.definition,
            query->adt_definition)
        || self_type->data.named_type.argument_count
            != item->generic_parameter_count
        || self_type->data.named_type.argument_count
            != query->argument_count
        || self_type->data.named_type.arguments == NULL) {
        return 0;
    }
    adt_item = cm_projection_bound_item(context,
        self_type->data.named_type.definition);
    if (adt_item == NULL
        || (adt_item->kind != CM_HIR_ITEM_STRUCT
            && adt_item->kind != CM_HIR_ITEM_ENUM)
        || adt_item->generic_parameter_count
            != item->generic_parameter_count
        || !cm_projection_type_parameter_range_is_type_only(context,
            adt_item, 0)) {
        return 0;
    }
    for (index = 0u; index < item->generic_parameter_count; ++index) {
        const CmHirGenericArg *argument;
        const CmHirType *argument_type;

        argument = &self_type->data.named_type.arguments[index];
        if (argument->kind != CM_HIR_GENERIC_ARG_TYPE
            || (argument_type = cm_hir_get_type(context,
                argument->data.type)) == NULL
            || argument_type->kind != CM_HIR_TYPE_PARAMETER_KIND
            || argument_type->data.parameter_type.parameter
                != item->generic_parameter_start + index) {
            return 0;
        }
    }
    return 1;
}

static int cm_projection_candidate_may_match(
    const CmHirContext *context, const CmHirItem *item,
    const CmHirType *query_type, const CmProjectionSelf *query)
{
    const CmHirType *candidate;
    CmProjectionKey candidate_key;

    candidate = cm_hir_get_type(context, item->data.impl_item.self_type);
    if (candidate == NULL) return 1;
    if (candidate->kind == CM_HIR_TYPE_PARAMETER_KIND) return 1;
    if (query->class_kind == CM_PROJECTION_SELF_GENERIC_ADT
        && candidate->kind == CM_HIR_TYPE_ADT_KIND) {
        return cm_hir_def_id_equal(candidate->data.named_type.definition,
            query->adt_definition);
    }
    if (query->class_kind == CM_PROJECTION_SELF_EXACT
        && cm_projection_scalar_key(candidate, &candidate_key)) {
        return cm_projection_key_equal(&candidate_key, &query->exact_key);
    }
    if (query->class_kind == CM_PROJECTION_SELF_EXACT
        && query->exact_key.kind == CM_HIR_TYPE_ADT_KIND
        && candidate->kind == CM_HIR_TYPE_ADT_KIND) {
        return cm_hir_def_id_equal(candidate->data.named_type.definition,
            query->exact_key.data.definition);
    }
    return candidate->kind == query_type->kind;
}

static CmProjectionCandidateMatch cm_projection_match_candidate(
    const CmHirContext *context, const CmHirItem *item,
    const CmHirType *query_type, const CmProjectionSelf *query,
    CmHirCrateId local_crate)
{
    CmProjectionKey candidate_key;

    if (item->data.impl_item.trait_type.argument_count != 0u
        || item->data.impl_item.trait_type.arguments != NULL) {
        return cm_projection_candidate_may_match(context, item, query_type,
            query) ? CM_PROJECTION_CANDIDATE_DEFERRED
                   : CM_PROJECTION_CANDIDATE_UNEQUAL;
    }
    if (item->generic_parameter_count == 0u) {
        if (query->class_kind == CM_PROJECTION_SELF_EXACT
            && cm_projection_exact_candidate_key(context, item,
                local_crate, &candidate_key)) {
            return cm_projection_key_equal(&candidate_key,
                &query->exact_key) ? CM_PROJECTION_CANDIDATE_EQUAL
                                   : CM_PROJECTION_CANDIDATE_UNEQUAL;
        }
        return cm_projection_candidate_may_match(context, item, query_type,
            query) ? CM_PROJECTION_CANDIDATE_DEFERRED
                   : CM_PROJECTION_CANDIDATE_UNEQUAL;
    }
    if (cm_projection_ordered_generic_candidate(context, item, query)) {
        return CM_PROJECTION_CANDIDATE_EQUAL;
    }
    return cm_projection_candidate_may_match(context, item, query_type,
        query) ? CM_PROJECTION_CANDIDATE_DEFERRED
               : CM_PROJECTION_CANDIDATE_UNEQUAL;
}

static int cm_projection_association_valid(const CmHirItem *trait_item,
    const CmHirItem *associated_item)
{
    return trait_item != NULL && trait_item->kind == CM_HIR_ITEM_TRAIT
        && associated_item != NULL
        && associated_item->kind == CM_HIR_ITEM_TYPE_ALIAS
        && associated_item->data.type_alias_item.target == CM_HIR_TYPE_NONE
        && cm_hir_def_id_is_none(associated_item->data.type_alias_item
                .trait_item_definition)
        && cm_hir_def_id_equal(associated_item->parent_definition,
            trait_item->definition);
}

static const CmHirItem *cm_projection_impl_associated_item(
    const CmHirContext *context, CmHirDefId impl_definition,
    CmHirDefId trait_item_definition, uint32_t *out_count)
{
    const CmHirItem *result;
    size_t index;

    result = NULL;
    *out_count = 0u;
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        if (item != NULL && item->kind == CM_HIR_ITEM_TYPE_ALIAS
            && cm_hir_def_id_equal(item->parent_definition,
                impl_definition)
            && cm_hir_def_id_equal(
                item->data.type_alias_item.trait_item_definition,
                trait_item_definition)) {
            result = item;
            *out_count += 1u;
        }
    }
    return result;
}

static int cm_projection_impl_contains_specializable_member(
    const CmHirContext *context, CmHirDefId impl_definition)
{
    size_t index;

    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        if (item != NULL && item->is_specializable
            && cm_hir_def_id_equal(item->parent_definition,
                impl_definition)) {
            return 1;
        }
    }
    return 0;
}

CmHirProjectionImplTarget cm_hir_projection_impl_target(
    const CmHirContext *context, CmHirCrateId local_crate,
    CmHirDefId impl_definition, CmHirDefId trait_definition,
    CmHirDefId trait_associated_definition)
{
    const CmHirItem *impl_item;
    const CmHirItem *trait_item;
    const CmHirItem *trait_associated;
    const CmHirItem *impl_associated;
    CmHirProjectionImplTarget result;
    uint32_t associated_count;

    if (context == NULL || cm_hir_get_crate(context, local_crate) == NULL
        || cm_hir_def_id_is_none(impl_definition)
        || cm_hir_def_id_is_none(trait_definition)
        || cm_hir_def_id_is_none(trait_associated_definition)) {
        return cm_projection_impl_target_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    if (impl_definition.crate_id != local_crate
        || trait_definition.crate_id != local_crate
        || trait_associated_definition.crate_id != local_crate) {
        return cm_projection_impl_target_result(
            CM_HIR_PROJECTION_DEFERRED_CRATE);
    }
    impl_item = cm_projection_bound_item(context, impl_definition);
    trait_item = cm_projection_bound_item(context, trait_definition);
    trait_associated = cm_projection_bound_item(context,
        trait_associated_definition);
    if (!cm_projection_association_valid(trait_item, trait_associated)
        || impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL
        || impl_item->data.impl_item.has_trait != 1
        || impl_item->data.impl_item.polarity != CM_HIR_IMPL_POSITIVE
        || !cm_hir_def_id_equal(
            impl_item->data.impl_item.trait_type.definition,
            trait_definition)) {
        return cm_projection_impl_target_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    if (trait_item->data.trait_item.is_auto
        || trait_associated->generic_parameter_count != 0u
        || !cm_projection_type_parameter_range_is_type_only(context,
            impl_item, 1)) {
        return cm_projection_impl_target_result(
            CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    }
    if (cm_projection_impl_contains_specializable_member(context,
            impl_definition)) {
        return cm_projection_impl_target_result(
            CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    }
    impl_associated = cm_projection_impl_associated_item(context,
        impl_definition, trait_associated_definition, &associated_count);
    if (associated_count != 1u || impl_associated == NULL
        || impl_associated->kind != CM_HIR_ITEM_TYPE_ALIAS
        || !cm_hir_def_id_equal(impl_associated->parent_definition,
            impl_definition)
        || !cm_hir_def_id_equal(impl_associated->data.type_alias_item
                .trait_item_definition,
            trait_associated_definition)
        || impl_associated->generic_parameter_count != 0u
        || impl_associated->data.type_alias_item.target
            == CM_HIR_TYPE_NONE) {
        return cm_projection_impl_target_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    if (impl_associated->definition.crate_id != local_crate) {
        return cm_projection_impl_target_result(
            CM_HIR_PROJECTION_DEFERRED_CRATE);
    }
    result = cm_projection_impl_target_result(CM_HIR_PROJECTION_SELECTED);
    result.target_template = impl_associated->data.type_alias_item.target;
    result.impl_associated_definition = impl_associated->definition;
    return result;
}

CmHirProjectionMatch cm_hir_match_projection(
    const CmHirContext *context, CmHirCrateId local_crate,
    CmHirTypeId projection_type)
{
    const CmHirType *projection;
    const CmHirType *self_type;
    const CmHirItem *trait_item;
    const CmHirItem *associated_item;
    const CmHirItem *selected_impl;
    const CmHirItem *selected_associated;
    CmProjectionSelf query;
    size_t index;
    uint32_t candidate_count;
    uint32_t associated_count;
    int saw_deferred_crate;
    int saw_deferred;
    int saw_specialization;

    if (context == NULL || cm_hir_get_crate(context, local_crate) == NULL) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    projection = cm_hir_get_type(context, projection_type);
    if (projection == NULL || projection->kind != CM_HIR_TYPE_PROJECTION_KIND) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    trait_item = cm_projection_bound_item(context,
        projection->data.projection_type.trait_type.definition);
    associated_item = cm_projection_bound_item(context,
        projection->data.projection_type.associated_type.definition);
    if (!cm_projection_association_valid(trait_item, associated_item)) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    if (trait_item->definition.crate_id != local_crate) {
        return cm_projection_match_result(CM_HIR_PROJECTION_DEFERRED_CRATE);
    }
    if ((projection->data.projection_type.trait_type.argument_count != 0u
            && projection->data.projection_type.trait_type.arguments == NULL)
        || (projection->data.projection_type.associated_type.argument_count
                != 0u
            && projection->data.projection_type.associated_type.arguments
                == NULL)) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    if (projection->data.projection_type.trait_type.argument_count != 0u
        || projection->data.projection_type.associated_type.argument_count
            != 0u
        || trait_item->generic_parameter_count != 0u
        || associated_item->generic_parameter_count != 0u) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    }
    self_type = cm_hir_get_type(context,
        projection->data.projection_type.self_type);
    if (self_type == NULL) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    query = cm_projection_query_self(context, self_type, local_crate);
    if (query.class_kind == CM_PROJECTION_SELF_FOREIGN) {
        return cm_projection_match_result(CM_HIR_PROJECTION_DEFERRED_CRATE);
    }
    if (query.class_kind == CM_PROJECTION_SELF_UNSUPPORTED) {
        return cm_projection_match_result(CM_HIR_PROJECTION_DEFERRED_SELF);
    }

    selected_impl = NULL;
    candidate_count = 0u;
    saw_deferred_crate = 0;
    saw_deferred = 0;
    saw_specialization = 0;
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;
        CmProjectionCandidateMatch candidate_match;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        if (item == NULL || item->kind != CM_HIR_ITEM_IMPL
            || item->data.impl_item.has_trait != 1
            || item->data.impl_item.polarity != CM_HIR_IMPL_POSITIVE
            || !cm_hir_def_id_equal(
                item->data.impl_item.trait_type.definition,
                trait_item->definition)) {
            continue;
        }
        if (item->definition.crate_id != local_crate) {
            if (cm_projection_candidate_may_match(context, item, self_type,
                    &query)) {
                saw_deferred_crate = 1;
            }
            continue;
        }
        candidate_match = cm_projection_match_candidate(context, item,
            self_type, &query, local_crate);
        if (candidate_match == CM_PROJECTION_CANDIDATE_EQUAL) {
            if (cm_projection_impl_contains_specializable_member(context,
                    item->definition)) {
                saw_specialization = 1;
            } else {
                selected_impl = item;
                candidate_count += 1u;
            }
        } else if (candidate_match == CM_PROJECTION_CANDIDATE_DEFERRED) {
            saw_deferred = 1;
        }
    }
    if (saw_specialization) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    }
    if (candidate_count > 1u) {
        return cm_projection_match_result(CM_HIR_PROJECTION_AMBIGUOUS);
    }
    if (saw_deferred_crate) {
        return cm_projection_match_result(CM_HIR_PROJECTION_DEFERRED_CRATE);
    }
    if (saw_deferred) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    }
    if (candidate_count == 0u || selected_impl == NULL) {
        return cm_projection_match_result(CM_HIR_PROJECTION_NO_IMPL);
    }
    selected_associated = cm_projection_impl_associated_item(context,
        selected_impl->definition, associated_item->definition,
        &associated_count);
    if (associated_count != 1u || selected_associated == NULL) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    if (selected_associated->generic_parameter_count != 0u) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_DEFERRED_ARGUMENTS);
    }
    if (selected_associated->data.type_alias_item.target
            == CM_HIR_TYPE_NONE) {
        return cm_projection_match_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    {
        CmHirProjectionMatch result;

        result = cm_projection_match_result(CM_HIR_PROJECTION_SELECTED);
        result.target_template =
            selected_associated->data.type_alias_item.target;
        result.query_self = projection->data.projection_type.self_type;
        result.impl_definition = selected_impl->definition;
        result.impl_associated_definition = selected_associated->definition;
        return result;
    }
}

CmHirProjectionResult cm_hir_select_projection(
    CmHirContext *context, CmHirCrateId local_crate,
    CmHirTypeId projection_type)
{
    CmHirProjectionMatch match;
    const CmHirItem *impl_item;
    const CmHirType *query_self;
    CmHirTypeAliasResult instantiated;
    CmHirProjectionResult result;

    match = cm_hir_match_projection(context, local_crate, projection_type);
    if (match.status != CM_HIR_PROJECTION_SELECTED) {
        return cm_projection_result(match.status);
    }
    impl_item = cm_projection_bound_item(context, match.impl_definition);
    if (impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL) {
        return cm_projection_result(
            CM_HIR_PROJECTION_INVALID_ASSOCIATION);
    }
    result = cm_projection_result(CM_HIR_PROJECTION_SELECTED);
    if (impl_item->generic_parameter_count == 0u) {
        result.target = match.target_template;
    } else {
        query_self = cm_hir_get_type(context, match.query_self);
        if (query_self == NULL || query_self->kind != CM_HIR_TYPE_ADT_KIND
            || query_self->data.named_type.argument_count
                != impl_item->generic_parameter_count
            || query_self->data.named_type.arguments == NULL) {
            return cm_projection_result(
                CM_HIR_PROJECTION_SUBSTITUTION_FAILURE);
        }
        instantiated = cm_hir_instantiate_type(context,
            match.target_template, impl_item->definition,
            query_self->data.named_type.arguments,
            query_self->data.named_type.argument_count);
        if (instantiated.status != CM_HIR_TYPE_ALIAS_OK) {
            result = cm_projection_result(
                CM_HIR_PROJECTION_SUBSTITUTION_FAILURE);
            result.hir_status = instantiated.hir_status;
            return result;
        }
        result.target = instantiated.type;
        result.allocated_type_count = instantiated.allocated_type_count;
    }
    result.impl_definition = match.impl_definition;
    result.impl_associated_definition = match.impl_associated_definition;
    return result;
}

const char *cm_hir_projection_status_name(CmHirProjectionStatus status)
{
    switch (status) {
    case CM_HIR_PROJECTION_SELECTED:
        return "selected";
    case CM_HIR_PROJECTION_DEFERRED_ARGUMENTS:
        return "deferred arguments";
    case CM_HIR_PROJECTION_DEFERRED_SELF:
        return "deferred self";
    case CM_HIR_PROJECTION_DEFERRED_CRATE:
        return "deferred crate";
    case CM_HIR_PROJECTION_NO_IMPL:
        return "no impl";
    case CM_HIR_PROJECTION_AMBIGUOUS:
        return "ambiguous";
    case CM_HIR_PROJECTION_SUBSTITUTION_FAILURE:
        return "substitution failure";
    case CM_HIR_PROJECTION_INVALID_ASSOCIATION:
        return "invalid association";
    }
    return "unknown projection status";
}
