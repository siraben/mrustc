#define _POSIX_C_SOURCE 200809L

#include "cm/compile.h"

#include "cm/alloc.h"
#include "cm/buf.h"
#include "cm/codegen/c.h"
#include "cm/driver/cfg.h"
#include "cm/hir/admission.h"
#include "cm/hir/body.h"
#include "cm/hir/lower.h"
#include "cm/hir/metadata.h"
#include "cm/hir/module_map.h"
#include "cm/hir/semantic_body.h"
#include "cm/hir/semantic_item.h"
#include "cm/hir/semantic_results.h"
#include "../hir/admission_internal.h"
#include "../hir/instance_internal.h"
#include "../hir/semantic_results_internal.h"
#include "cm/macro/expand.h"
#include "cm/mir/lower.h"
#include "cm/mir/model.h"
#include "cm/resolve/imports.h"
#include "cm/resolve/module_graph.h"
#include "cm/source.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static CmCompileResult cm_compile_result(CmCompileStatus status,
    const char *message)
{
    CmCompileResult result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    if (message != NULL) {
        (void)snprintf(result.message, sizeof(result.message), "%s", message);
    }
    return result;
}

static int cm_compile_open_temporary(char *path)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t length;
    unsigned long attempt;

    if (path == NULL) return -1;
    length = strlen(path);
    if (length < 6u) return -1;
    for (attempt = 0u; attempt < 1024u; ++attempt) {
        uint64_t value;
        size_t digit;
        int descriptor;

        value = (uint64_t)(unsigned long)getpid() * UINT64_C(1103515245)
            + (uint64_t)attempt;
        for (digit = 0u; digit < 6u; ++digit) {
            path[length - digit - 1u] = alphabet[value % 36u];
            value /= 36u;
        }
        descriptor = open(path, O_RDWR | O_CREAT | O_EXCL,
            S_IRUSR | S_IWUSR);
        if (descriptor >= 0) return descriptor;
        if (errno != EEXIST) return -1;
    }
    return -1;
}

static int cm_compile_hir_edition(enum cm_edition edition,
    CmHirEdition *out_edition)
{
    switch (edition) {
    case CM_EDITION_2015: *out_edition = CM_HIR_EDITION_2015; return 1;
    case CM_EDITION_2018: *out_edition = CM_HIR_EDITION_2018; return 1;
    case CM_EDITION_2021: *out_edition = CM_HIR_EDITION_2021; return 1;
    case CM_EDITION_2024: *out_edition = CM_HIR_EDITION_2024; return 1;
    }
    return 0;
}

static int cm_compile_identifier_valid(const char *identifier)
{
    const unsigned char *bytes;
    size_t index;

    if (identifier == NULL || identifier[0] == '\0') return 0;
    bytes = (const unsigned char *)identifier;
    if (!((bytes[0] >= (unsigned char)'a'
                && bytes[0] <= (unsigned char)'z')
            || (bytes[0] >= (unsigned char)'A'
                && bytes[0] <= (unsigned char)'Z')
            || bytes[0] == (unsigned char)'_')) return 0;
    for (index = 1u; bytes[index] != (unsigned char)'\0'; ++index) {
        if (!((bytes[index] >= (unsigned char)'a'
                    && bytes[index] <= (unsigned char)'z')
                || (bytes[index] >= (unsigned char)'A'
                    && bytes[index] <= (unsigned char)'Z')
                || (bytes[index] >= (unsigned char)'0'
                    && bytes[index] <= (unsigned char)'9')
                || bytes[index] == (unsigned char)'_')) return 0;
    }
    return 1;
}

static int cm_compile_same_file(const struct stat *left,
    const struct stat *right)
{
    return left != NULL && right != NULL
        && left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static int cm_compile_hir_name_is(const CmHirContext *hir, CmInternId name,
    const char *expected)
{
    const CmInternedString *text;
    size_t length;

    text = cm_interner_get(&hir->strings, name);
    length = strlen(expected);
    return text != NULL && text->len == length
        && memcmp(text->bytes, expected, length) == 0;
}

static CmHirItemId cm_compile_find_entry(const CmHirContext *hir,
    CmHirModuleId root_module)
{
    CmHirItemId found;
    size_t index;

    found = CM_HIR_ITEM_NONE;
    for (index = 0u; index < hir->items.len; ++index) {
        const CmHirItem *item;
        CmHirItemId id;

        item = (const CmHirItem *)cm_vec_at_const(&hir->items, index);
        id = (CmHirItemId)(index + 1u);
        if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
            || item->owner_module != root_module
            || !cm_hir_def_id_is_none(item->parent_definition)
            || !cm_compile_hir_name_is(hir, item->name, "main")) {
            continue;
        }
        if (found != CM_HIR_ITEM_NONE) return CM_HIR_ITEM_NONE;
        found = id;
    }
    return found;
}

typedef struct CmCompileReachableInstance {
    CmHirCanonicalInstance identity;
    CmHirDefId definition;
    CmHirTypeId substitution;
    uint32_t substitution_count;
    CmHirBodyId body;
    size_t edge_start;
    size_t edge_count;
    CmMirBodyId mir_body;
} CmCompileReachableInstance;

typedef struct CmCompileReachableEdge {
    size_t callee;
    CmHirExprId expression;
    CmHirCanonicalInstance callee_identity;
} CmCompileReachableEdge;

typedef struct CmCompileExactState {
    CmHirContext *hir;
    CmMirContext *mir;
    CmVec instances;
    CmVec edges;
    const CmSemanticAdmission *all_local_admission;
    CmHirCrateId local_crate;
    uint64_t all_local_generation;
    uint64_t all_local_capability_id;
    const CmSemanticAdmission *admission;
} CmCompileExactState;

static int cm_compile_all_local_admission_current(
    const CmSemanticAdmission *admission, const CmHirContext *hir,
    CmHirCrateId local_crate, uint64_t generation, uint64_t capability_id)
{
    return admission != NULL && hir != NULL
        && local_crate != CM_HIR_CRATE_NONE && generation != UINT64_C(0)
        && capability_id != UINT64_C(0)
        && cm_semantic_admission_is_current(admission)
        && cm_semantic_admission_hir(admission) == hir
        && cm_semantic_admission_crate(admission) == local_crate
        && cm_semantic_admission_generation(admission) == generation
        && cm_semantic_admission_capability_id(admission) == capability_id;
}

static int cm_compile_exact_all_local_current(
    const CmCompileExactState *state)
{
    return state != NULL && cm_compile_all_local_admission_current(
        state->all_local_admission, state->hir, state->local_crate,
        state->all_local_generation, state->all_local_capability_id);
}

static int cm_compile_all_local_body_admitted(
    const CmCompileExactState *state, CmHirBodyId body_id)
{
    const CmSemanticResults *results;
    CmSemanticBodyView view;

    if (!cm_compile_exact_all_local_current(state)) return 0;
    results = cm_semantic_admission_results(state->all_local_admission);
    return results != NULL
        && cm_semantic_results_body(results, state->all_local_admission,
            body_id, &view) == CM_SEMANTIC_RESULTS_OK
        && view.body == body_id;
}

static int cm_compile_item_has_attribute(const CmHirContext *hir,
    const CmHirItem *item, const char *name)
{
    uint32_t index;

    for (index = 0u; index < item->attribute_count; ++index) {
        if (cm_compile_hir_name_is(hir,
                item->attributes[index].metadata, name)) {
            return 1;
        }
    }
    return 0;
}

static int cm_compile_is_export_root(const CmHirContext *hir,
    const CmHirItem *item)
{
    return item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && cm_hir_def_id_is_none(item->parent_definition)
        && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && cm_hir_def_id_is_none(item->visibility.restriction)
        && cm_compile_item_has_attribute(hir, item, "no_mangle");
}

static const CmHirItem *cm_compile_definition_item(
    const CmHirContext *hir, CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
        || record->state != CM_HIR_DEFINITION_BOUND ? NULL
        : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static const CmHirItem *cm_compile_definition_any_item(
    const CmHirContext *hir, CmHirDefId definition)
{
    const CmHirDefinition *record;
    const CmHirItem *item;

    record = cm_hir_lookup_definition(hir, definition);
    item = record == NULL || record->kind != CM_HIR_DEFINITION_ITEM
            || record->state != CM_HIR_DEFINITION_BOUND
        ? NULL : cm_hir_get_item(hir, record->entity.item_id);
    return item != NULL && cm_hir_def_id_equal(item->definition, definition)
        ? item : NULL;
}

static int cm_compile_instance_spec_for_item(const CmHirContext *hir,
    const CmHirItem *item, const CmHirTypeId *substitutions,
    uint32_t substitution_count, CmHirInstanceSpec *out_spec,
    CmHirGenericArg *out_argument)
{
    const CmHirItem *impl_item;

    if (hir == NULL || item == NULL || out_spec == NULL
        || out_argument == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || (substitution_count == 0u) != (substitutions == NULL)) return 0;
    cm_hir_instance_spec_init(out_spec);
    memset(out_argument, 0, sizeof(*out_argument));
    out_spec->selected_callable = item->definition;
    if (cm_hir_def_id_is_none(item->parent_definition)) {
        if (!cm_hir_def_id_is_none(
                item->data.function_item.trait_item_definition)
            || substitution_count > 1u) return 0;
        if (substitution_count != 0u) {
            out_argument->kind = CM_HIR_GENERIC_ARG_TYPE;
            out_argument->data.type = substitutions[0];
            out_spec->item_arguments = out_argument;
            out_spec->item_argument_count = 1u;
        }
        return 1;
    }
    if (substitution_count != 0u
        || item->generic_parameter_count != 0u) return 0;
    impl_item = cm_compile_definition_any_item(hir,
        item->parent_definition);
    if (impl_item == NULL || impl_item->kind != CM_HIR_ITEM_IMPL
        || impl_item->generic_parameter_count != 0u
        || !impl_item->data.impl_item.has_trait
        || impl_item->data.impl_item.is_negative
        || cm_hir_def_id_is_none(
            item->data.function_item.trait_item_definition)) return 0;
    out_spec->declared_trait_callable =
        item->data.function_item.trait_item_definition;
    out_spec->enclosing_impl = impl_item->definition;
    out_spec->implemented_trait =
        impl_item->data.impl_item.trait_type.definition;
    out_spec->self_owner = impl_item->definition;
    out_spec->self_type = impl_item->data.impl_item.self_type;
    return 1;
}

static int cm_compile_type_is_u32(const CmHirContext *hir,
    CmHirTypeId type_id)
{
    const CmHirType *type;

    type = cm_hir_get_type(hir, type_id);
    return type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U32;
}

static int cm_compile_method_trait_in_scope(const CmHirExpr *expression,
    CmHirDefId trait_definition)
{
    uint32_t index;

    if (expression == NULL || expression->kind != CM_HIR_EXPR_METHOD_CALL) {
        return 0;
    }
    for (index = 0u;
         index < expression->data.method_call.in_scope_trait_count; ++index) {
        if (cm_hir_def_id_equal(
                expression->data.method_call.in_scope_traits[index],
                trait_definition)) return 1;
    }
    return 0;
}

static int cm_compile_callable_arguments(const CmHirExpr *expression,
    CmHirExprId storage[2], const CmHirExprId **out_arguments,
    uint32_t *out_count)
{
    uint32_t index;

    if (expression == NULL || storage == NULL || out_arguments == NULL
        || out_count == NULL) return 0;
    if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
        if (expression->data.qualified_call.argument_count == 0u
            || expression->data.qualified_call.argument_count > 2u
            || expression->data.qualified_call.arguments == NULL) return 0;
        *out_arguments = expression->data.qualified_call.arguments;
        *out_count = expression->data.qualified_call.argument_count;
        return 1;
    }
    if (expression->kind != CM_HIR_EXPR_METHOD_CALL
        || expression->data.method_call.receiver == CM_HIR_EXPR_NONE
        || expression->data.method_call.argument_count > 1u
        || (expression->data.method_call.argument_count != 0u
            && expression->data.method_call.arguments == NULL)) return 0;
    storage[0] = expression->data.method_call.receiver;
    for (index = 0u; index < expression->data.method_call.argument_count;
         ++index) {
        storage[index + 1u] = expression->data.method_call.arguments[index];
    }
    *out_arguments = storage;
    *out_count = expression->data.method_call.argument_count + 1u;
    return 1;
}

static int cm_compile_semantic_types_equal(const CmSemanticTypeView *left,
    const CmSemanticTypeView *right)
{
    int equal;

    equal = 0;
    return cm_semantic_type_view_equal(left, right, &equal)
            == CM_SEMANTIC_RESULTS_OK
        && equal;
}

static int cm_compile_callable_selection(
    const CmCompileExactState *state, const CmHirExpr *expression,
    CmHirExprId expression_id, CmSemanticCallableSelectionView *out_view,
    char *message, size_t message_capacity)
{
    const CmSemanticResults *results;
    const CmHirExpr *receiver;
    const CmHirItem *declared;
    const CmHirExprId *arguments;
    CmHirExprId argument_storage[2];
    CmSemanticExpressionView expression_view;
    CmSemanticExpressionView receiver_view;
    uint32_t argument_count;
    int self_matches;
    uint32_t index;

    results = state == NULL ? NULL
        : cm_semantic_admission_results(state->all_local_admission);
    receiver = NULL;
    declared = NULL;
    arguments = NULL;
    argument_count = 0u;
    memset(&expression_view, 0, sizeof(expression_view));
    memset(&receiver_view, 0, sizeof(receiver_view));
    self_matches = 0;
    if (results == NULL || expression == NULL
        || (expression->kind != CM_HIR_EXPR_QUALIFIED_CALL
            && expression->kind != CM_HIR_EXPR_METHOD_CALL)
        || !cm_compile_callable_arguments(expression, argument_storage,
            &arguments, &argument_count)
        || cm_semantic_results_callable_selection(results,
            state->all_local_admission, expression->owner_body,
            expression_id, out_view) != CM_SEMANTIC_RESULTS_OK
        || out_view->body != expression->owner_body
        || out_view->expression != expression_id
        || cm_hir_def_id_is_none(out_view->selected_impl)
        || cm_hir_def_id_is_none(out_view->selected_callable)
        || out_view->argument_count != argument_count
        || cm_semantic_results_expression(results,
            state->all_local_admission, expression->owner_body,
            expression_id, &expression_view) != CM_SEMANTIC_RESULTS_OK
        || expression_view.adjustment_count != 0u
        || !cm_compile_semantic_types_equal(&expression_view.unadjusted_type,
            &expression_view.adjusted_type)) goto invalid;
    if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL) {
        if (out_view->syntax != expression->data.qualified_call.syntax
            || out_view->syntax
                != CM_HIR_CALLABLE_QUALIFIED_TRAIT_METHOD
            || !cm_hir_def_id_equal(out_view->requested_trait,
                expression->data.qualified_call.requested_trait)
            || !cm_hir_def_id_equal(out_view->declared_trait_callable,
                expression->data.qualified_call.declared_trait_callable)
            || out_view->receiver_argument
                != expression->data.qualified_call.receiver_argument
            || cm_semantic_type_view_matches_monomorphic_hir(results,
                state->all_local_admission, &out_view->requested_self_type,
                expression->data.qualified_call.requested_self_type,
                &self_matches) != CM_SEMANTIC_RESULTS_OK
            || !self_matches) goto invalid;
    } else {
        receiver = cm_hir_get_expr(state->hir,
            expression->data.method_call.receiver);
        declared = cm_compile_definition_item(state->hir,
            out_view->declared_trait_callable);
        if (out_view->syntax != expression->data.method_call.syntax
            || out_view->syntax != CM_HIR_CALLABLE_DOT_METHOD
            || out_view->receiver_argument != 0u
            || out_view->receiver_expression
                != expression->data.method_call.receiver
            || !cm_compile_method_trait_in_scope(expression,
                out_view->requested_trait)
            || declared == NULL
            || declared->name != expression->data.method_call.method_name
            || !cm_hir_def_id_equal(declared->parent_definition,
                out_view->requested_trait)
            || declared->data.function_item.signature.receiver
                != CM_HIR_RECEIVER_VALUE
            || receiver == NULL || receiver->owner_body != expression->owner_body
            || cm_semantic_results_expression(results,
                state->all_local_admission, expression->owner_body,
                expression->data.method_call.receiver, &receiver_view)
                    != CM_SEMANTIC_RESULTS_OK
            || receiver_view.adjustment_count != 0u
            || !cm_compile_semantic_types_equal(
                &receiver_view.unadjusted_type, &receiver_view.adjusted_type)
            || !cm_compile_semantic_types_equal(
                &out_view->requested_self_type,
                &receiver_view.adjusted_type)
            || cm_semantic_type_view_matches_monomorphic_hir(results,
                state->all_local_admission, &out_view->requested_self_type,
                receiver->type, &self_matches) != CM_SEMANTIC_RESULTS_OK
            || !self_matches) goto invalid;
    }
    if (out_view->receiver_argument == CM_HIR_CALLABLE_RECEIVER_NONE) {
        if (out_view->receiver_expression != CM_HIR_EXPR_NONE) goto invalid;
    } else if (out_view->receiver_argument >= out_view->argument_count
        || out_view->receiver_expression
            != arguments[out_view->receiver_argument]) goto invalid;
    for (index = 0u; index < out_view->argument_count; ++index) {
        CmHirExprId argument;
        CmSemanticExpressionView argument_view;

        memset(&argument_view, 0, sizeof(argument_view));
        if (cm_semantic_results_callable_argument(results,
                state->all_local_admission, expression->owner_body,
                expression_id, index, &argument)
                != CM_SEMANTIC_RESULTS_OK
            || argument != arguments[index]
            || cm_semantic_results_expression(results,
                state->all_local_admission, expression->owner_body,
                argument, &argument_view) != CM_SEMANTIC_RESULTS_OK
            || argument_view.adjustment_count != 0u
            || !cm_compile_semantic_types_equal(
                &argument_view.unadjusted_type,
                &argument_view.adjusted_type)) {
            goto invalid;
        }
    }
    return 1;

invalid:
    (void)snprintf(message, message_capacity,
        "reachable selected call lacks a matching sealed callable recipe");
    return 0;
}

static int cm_compile_callable_executable_substitution(
    const CmCompileExactState *state, const CmHirExpr *expression,
    CmHirExprId expression_id,
    const CmSemanticCallableSelectionView *selection,
    CmHirTypeId *out_substitution, uint32_t *out_substitution_count,
    char *message, size_t message_capacity)
{
    const CmSemanticResults *results;
    CmSemanticGenericArgumentView argument;

    if (state == NULL || expression == NULL || selection == NULL
        || out_substitution == NULL || out_substitution_count == NULL) {
        return 0;
    }
    *out_substitution = CM_HIR_TYPE_NONE;
    *out_substitution_count = 0u;
    if (selection->item_argument_count != 0u
        || selection->method_argument_count != 0u
        || selection->implemented_trait_argument_count != 0u
        || selection->enclosing_impl_argument_count > 1u) {
        goto unsupported;
    }
    if (selection->enclosing_impl_argument_count == 0u) return 1;
    results = cm_semantic_admission_results(state->all_local_admission);
    memset(&argument, 0, sizeof(argument));
    if (results == NULL
        || cm_semantic_results_callable_generic_argument(results,
            state->all_local_admission, expression->owner_body,
            expression_id,
            CM_SEMANTIC_CALLABLE_GENERIC_ARGUMENT_ENCLOSING_IMPL, 0u,
            &argument) != CM_SEMANTIC_RESULTS_OK
        || argument.kind != CM_HIR_GENERIC_ARG_TYPE
        || cm_semantic_type_view_materialize_existing_hir(results,
            state->all_local_admission, &argument.normalized,
            out_substitution) != CM_SEMANTIC_RESULTS_OK) {
        goto unsupported;
    }
    *out_substitution_count = 1u;
    return 1;

unsupported:
    *out_substitution = CM_HIR_TYPE_NONE;
    *out_substitution_count = 0u;
    (void)snprintf(message, message_capacity,
        "reachable selected-call executable substitutions are unsupported");
    return 0;
}

static int cm_compile_materialize_call_substitutions(
    const CmCompileExactState *state, size_t caller_index,
    const CmHirExpr *expression, CmHirTypeId *substitution,
    uint32_t *substitution_count, char *message, size_t message_capacity)
{
    const CmCompileReachableInstance *caller;
    const CmHirType *type;
    const CmHirGenericParam *parameter;
    CmHirTypeId raw_type;

    *substitution_count = 0u;
    caller = (const CmCompileReachableInstance *)cm_vec_at_const(
        &state->instances, caller_index);
    if (caller == NULL || expression == NULL
        || expression->kind != CM_HIR_EXPR_CALL
        || expression->data.call.type_substitution_count > 1u
        || (expression->data.call.type_substitution_count != 0u
            && expression->data.call.type_substitutions == NULL)) {
        goto unsupported;
    }
    if (expression->data.call.type_substitution_count == 0u) return 1;
    raw_type = expression->data.call.type_substitutions[0];
    if (cm_compile_type_is_u32(state->hir, raw_type)) {
        *substitution = raw_type;
        *substitution_count = 1u;
        return 1;
    }
    type = cm_hir_get_type(state->hir, raw_type);
    parameter = type == NULL || type->kind != CM_HIR_TYPE_PARAMETER_KIND
        ? NULL : cm_hir_get_generic_param(state->hir,
            type->data.parameter_type.parameter);
    if (parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(parameter->owner, caller->definition)
        && parameter->index < caller->substitution_count
        && parameter->index == 0u
        && cm_compile_type_is_u32(state->hir, caller->substitution)) {
        *substitution = caller->substitution;
        *substitution_count = 1u;
        return 1;
    }

unsupported:
    (void)snprintf(message, message_capacity,
        "reachable call substitutions cannot be materialized");
    return 0;
}

static int cm_compile_intern_exact(CmCompileExactState *state,
    const CmHirItem *item, const CmHirTypeId *substitutions,
    uint32_t substitution_count, size_t *out_instance,
    char *message, size_t message_capacity)
{
    CmCompileReachableInstance instance;
    CmHirCanonicalInstance identity;
    CmHirInstanceSpec spec;
    CmHirGenericArg argument;
    size_t index;

    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || item->data.function_item.body == CM_HIR_BODY_NONE
        || item->generic_parameter_count != substitution_count
        || substitution_count > 1u
        || (substitution_count != 0u
            && (substitutions == NULL
                || !cm_compile_type_is_u32(state->hir,
                    substitutions[0])))) {
        (void)snprintf(message, message_capacity,
            "unsupported or invalid reachable function instance");
        return 0;
    }
    if (!cm_compile_instance_spec_for_item(state->hir, item,
            substitutions, substitution_count, &spec, &argument)) {
        (void)snprintf(message, message_capacity,
            "reachable function instance shape is unsupported");
        return 0;
    }
    cm_hir_canonical_instance_init(&identity);
    if (cm_hir_canonical_instance_encode(state->hir,
            item->definition.crate_id, &spec, &identity)
            != CM_HIR_INSTANCE_OK
        || identity.body != item->data.function_item.body) {
        cm_hir_canonical_instance_destroy(&identity);
        (void)snprintf(message, message_capacity,
            "reachable function instance is not canonical");
        return 0;
    }
    for (index = 0u; index < state->instances.len; ++index) {
        const CmCompileReachableInstance *candidate;
        int equal;

        candidate = (const CmCompileReachableInstance *)cm_vec_at_const(
            &state->instances, index);
        equal = 0;
        if (candidate != NULL
            && cm_hir_canonical_instance_equal(&candidate->identity,
                &identity, &equal) == CM_HIR_INSTANCE_OK && equal) {
            cm_hir_canonical_instance_destroy(&identity);
            *out_instance = index;
            return 1;
        }
    }
    memset(&instance, 0, sizeof(instance));
    instance.identity = identity;
    instance.definition = item->definition;
    instance.substitution_count = substitution_count;
    if (substitution_count != 0u) {
        instance.substitution = substitutions[0];
    }
    instance.body = item->data.function_item.body;
    instance.mir_body = CM_MIR_BODY_NONE;
    (void)cm_vec_push(&state->instances, &instance);
    *out_instance = state->instances.len - 1u;
    return 1;
}

static int cm_compile_intern_canonical(CmCompileExactState *state,
    const CmHirCanonicalInstance *identity, CmHirTypeId substitution,
    uint32_t substitution_count, size_t *out_instance,
    char *message, size_t message_capacity)
{
    CmCompileReachableInstance instance;
    const CmHirItem *item;
    size_t index;

    item = identity == NULL ? NULL
        : cm_compile_definition_item(state->hir, identity->definition);
    if (item == NULL || item->kind != CM_HIR_ITEM_FUNCTION
        || identity->body != item->data.function_item.body
        || cm_hir_canonical_instance_validate(state->hir,
            state->local_crate, identity) != CM_HIR_INSTANCE_OK
        || substitution_count > 1u
        || (substitution_count != 0u
            && substitution == CM_HIR_TYPE_NONE)) {
        (void)snprintf(message, message_capacity,
            "retained reachable instance is not canonical");
        return 0;
    }
    for (index = 0u; index < state->instances.len; ++index) {
        const CmCompileReachableInstance *candidate;
        int equal;

        candidate = (const CmCompileReachableInstance *)cm_vec_at_const(
            &state->instances, index);
        equal = 0;
        if (candidate != NULL
            && cm_hir_canonical_instance_equal(&candidate->identity,
                identity, &equal) == CM_HIR_INSTANCE_OK && equal) {
            if (candidate->substitution_count != substitution_count
                || (substitution_count != 0u
                    && candidate->substitution != substitution)) {
                (void)snprintf(message, message_capacity,
                    "canonical instance has inconsistent executable material");
                return 0;
            }
            *out_instance = index;
            return 1;
        }
    }
    memset(&instance, 0, sizeof(instance));
    cm_hir_canonical_instance_init(&instance.identity);
    if (cm_hir_canonical_instance_clone(&instance.identity, identity)
            != CM_HIR_INSTANCE_OK) {
        (void)snprintf(message, message_capacity,
            "canonical instance identity cannot be retained");
        return 0;
    }
    instance.definition = identity->definition;
    instance.substitution = substitution;
    instance.substitution_count = substitution_count;
    instance.body = identity->body;
    instance.mir_body = CM_MIR_BODY_NONE;
    (void)cm_vec_push(&state->instances, &instance);
    *out_instance = state->instances.len - 1u;
    return 1;
}

static int cm_compile_discover_expression_callees(
    CmCompileExactState *state, size_t caller_index,
    CmHirExprId expression_id, size_t depth, char *message,
    size_t message_capacity);

static int cm_compile_discover_expression_callees(
    CmCompileExactState *state, size_t caller_index,
    CmHirExprId expression_id, size_t depth, char *message,
    size_t message_capacity)
{
    const CmHirExpr *expression;
    CmHirExpr stable_expression;
    uint32_t index;

    if (depth >= state->hir->expressions.len) {
        (void)snprintf(message, message_capacity,
            "reachable expression tree is cyclic or too deep");
        return 0;
    }
    expression = cm_hir_get_expr(state->hir, expression_id);
    if (expression == NULL) {
        (void)snprintf(message, message_capacity,
            "reachable expression tree contains an invalid expression");
        return 0;
    }
    stable_expression = *expression;
    expression = &stable_expression;
    switch (expression->kind) {
    case CM_HIR_EXPR_INTEGER:
    case CM_HIR_EXPR_LOCAL:
        return 1;
    case CM_HIR_EXPR_BLOCK:
        if ((expression->data.block.statement_count == 0u)
                != (expression->data.block.statements == NULL)
            || expression->data.block.tail_expression >= expression_id) {
            break;
        }
        for (index = 0u; index < expression->data.block.statement_count;
             ++index) {
            const CmHirStatement *statement;

            statement = &expression->data.block.statements[index];
            if (statement->kind != CM_HIR_STATEMENT_LET
                || statement->data.let_statement.initializer
                    >= expression_id
                || !cm_compile_discover_expression_callees(state,
                    caller_index, statement->data.let_statement.initializer,
                    depth + 1u, message, message_capacity)) {
                if (message[0] == '\0') break;
                return 0;
            }
        }
        if (index != expression->data.block.statement_count) break;
        return cm_compile_discover_expression_callees(state, caller_index,
            expression->data.block.tail_expression, depth + 1u,
            message, message_capacity);
    case CM_HIR_EXPR_BINARY:
        if (expression->data.binary.left >= expression_id
            || expression->data.binary.right >= expression_id) {
            break;
        }
        return cm_compile_discover_expression_callees(state, caller_index,
                expression->data.binary.left, depth + 1u, message,
                message_capacity)
            && cm_compile_discover_expression_callees(state, caller_index,
                expression->data.binary.right, depth + 1u, message,
                message_capacity);
    case CM_HIR_EXPR_IF:
        if (expression->data.if_expr.condition >= expression_id
            || expression->data.if_expr.then_expression >= expression_id
            || expression->data.if_expr.else_expression >= expression_id) {
            break;
        }
        return cm_compile_discover_expression_callees(state, caller_index,
                expression->data.if_expr.condition, depth + 1u, message,
                message_capacity)
            && cm_compile_discover_expression_callees(state, caller_index,
                expression->data.if_expr.then_expression, depth + 1u,
                message, message_capacity)
            && cm_compile_discover_expression_callees(state, caller_index,
                expression->data.if_expr.else_expression, depth + 1u,
                message, message_capacity);
    case CM_HIR_EXPR_AGGREGATE:
        if (expression->data.aggregate.field_count == 0u
            || expression->data.aggregate.field_count
                > CM_MIR_MAX_AGGREGATE_FIELDS
            || expression->data.aggregate.fields == NULL) {
            break;
        }
        for (index = 0u; index < expression->data.aggregate.field_count;
             ++index) {
            CmHirExprId value;

            value = expression->data.aggregate.fields[index].value;
            if (value >= expression_id
                || !cm_compile_discover_expression_callees(state,
                    caller_index, value, depth + 1u, message,
                    message_capacity)) {
                if (message[0] == '\0') break;
                return 0;
            }
        }
        if (index == expression->data.aggregate.field_count) return 1;
        break;
    case CM_HIR_EXPR_FIELD:
        if (expression->data.field.base >= expression_id) break;
        return cm_compile_discover_expression_callees(state, caller_index,
            expression->data.field.base, depth + 1u, message,
            message_capacity);
    case CM_HIR_EXPR_BORROW_SHARED:
    case CM_HIR_EXPR_DEREFERENCE:
        (void)snprintf(message, message_capacity,
            "reachable reference expressions require semantic evidence");
        return 0;
    case CM_HIR_EXPR_METHOD_CALL:
    case CM_HIR_EXPR_QUALIFIED_CALL:
    {
        const CmHirItem *callee;
        const CmHirItem *impl_item;
        const CmSemanticResults *results;
        const CmHirExprId *call_arguments;
        CmHirExprId argument_storage[2];
        CmSemanticCallableSelectionView selection;
        CmCompileReachableEdge edge;
        CmHirCanonicalInstance selected_identity;
        CmHirTypeId executable_type;
        uint32_t executable_type_count;
        uint32_t call_argument_count;

        memset(&selection, 0, sizeof(selection));
        cm_hir_canonical_instance_init(&selected_identity);
        results = cm_semantic_admission_results(
            state->all_local_admission);
        call_arguments = NULL;
        call_argument_count = 0u;
        executable_type = CM_HIR_TYPE_NONE;
        executable_type_count = 0u;
        if (!cm_compile_callable_arguments(expression, argument_storage,
                &call_arguments, &call_argument_count)
            || !cm_compile_callable_selection(state, expression,
                expression_id, &selection, message, message_capacity)) {
            cm_hir_canonical_instance_destroy(&selected_identity);
            if (message[0] == '\0') {
                (void)snprintf(message, message_capacity,
                    "reachable selected call has an unsupported argument shape");
            }
            return 0;
        }
        for (index = 0u; index < call_argument_count; ++index) {
            if (call_arguments[index] >= expression_id
                || !cm_compile_discover_expression_callees(state,
                    caller_index, call_arguments[index],
                    depth + 1u, message, message_capacity)) {
                cm_hir_canonical_instance_destroy(&selected_identity);
                if (message[0] == '\0') {
                    (void)snprintf(message, message_capacity,
                        "reachable selected call argument discovery failed");
                }
                return 0;
            }
        }
        callee = cm_compile_definition_item(state->hir,
            selection.selected_callable);
        impl_item = callee == NULL ? NULL
            : cm_compile_definition_any_item(state->hir,
                callee->parent_definition);
        if (callee == NULL || impl_item == NULL
            || impl_item->kind != CM_HIR_ITEM_IMPL
            || !cm_hir_def_id_equal(impl_item->definition,
                selection.selected_impl)
            || !impl_item->data.impl_item.has_trait
            || impl_item->data.impl_item.is_negative
            || !cm_hir_def_id_equal(
                impl_item->data.impl_item.trait_type.definition,
                selection.requested_trait)
            || results == NULL
            || cm_semantic_results_callable_callee_identity(results,
                state->all_local_admission,
                expression->owner_body, expression_id,
                &selected_identity) != CM_SEMANTIC_RESULTS_OK
            || !cm_compile_callable_executable_substitution(state,
                expression, expression_id, &selection, &executable_type,
                &executable_type_count, message, message_capacity)
            || !cm_hir_def_id_equal(
                callee->data.function_item.trait_item_definition,
                selection.declared_trait_callable)
            || !cm_compile_intern_canonical(state, &selected_identity,
                executable_type, executable_type_count,
                &edge.callee, message, message_capacity)) {
            cm_hir_canonical_instance_destroy(&selected_identity);
            if (message[0] == '\0') {
                (void)snprintf(message, message_capacity,
                    "reachable callable selection is inconsistent with HIR");
            }
            return 0;
        }
        {
            const CmCompileReachableInstance *callee_instance;

            callee_instance =
                (const CmCompileReachableInstance *)cm_vec_at_const(
                    &state->instances, edge.callee);
            cm_hir_canonical_instance_init(&edge.callee_identity);
            if (callee_instance == NULL
                || cm_hir_canonical_instance_clone(&edge.callee_identity,
                    &callee_instance->identity) != CM_HIR_INSTANCE_OK) {
                cm_hir_canonical_instance_destroy(&selected_identity);
                cm_hir_canonical_instance_destroy(&edge.callee_identity);
                (void)snprintf(message, message_capacity,
                    "reachable callable identity cannot be retained");
                return 0;
            }
        }
        cm_hir_canonical_instance_destroy(&selected_identity);
        edge.expression = expression_id;
        (void)cm_vec_push(&state->edges, &edge);
        return 1;
    }
    case CM_HIR_EXPR_CALL:
    {
        const CmHirItem *callee;
        CmCompileReachableEdge edge;
        CmHirTypeId substitution;
        uint32_t substitution_count;

        if (expression->data.call.argument_count == 0u
            || expression->data.call.argument_count > 2u
            || expression->data.call.arguments == NULL
            || !cm_compile_materialize_call_substitutions(state,
                caller_index, expression, &substitution,
                &substitution_count, message, message_capacity)) {
            break;
        }
        for (index = 0u; index < expression->data.call.argument_count;
             ++index) {
            if (expression->data.call.arguments[index] >= expression_id
                || !cm_compile_discover_expression_callees(state,
                    caller_index, expression->data.call.arguments[index],
                    depth + 1u, message, message_capacity)) {
                if (message[0] == '\0') break;
                return 0;
            }
        }
        if (index != expression->data.call.argument_count) break;
        callee = cm_compile_definition_item(state->hir,
            expression->data.call.callee);
        if (callee != NULL
            && cm_compile_intern_exact(state, callee,
                substitution_count == 0u ? NULL : &substitution,
                substitution_count,
                &edge.callee, message, message_capacity)) {
            const CmCompileReachableInstance *callee_instance;

            callee_instance =
                (const CmCompileReachableInstance *)cm_vec_at_const(
                    &state->instances, edge.callee);
            cm_hir_canonical_instance_init(&edge.callee_identity);
            if (callee_instance == NULL
                || cm_hir_canonical_instance_clone(&edge.callee_identity,
                    &callee_instance->identity) != CM_HIR_INSTANCE_OK) {
                cm_hir_canonical_instance_destroy(&edge.callee_identity);
                (void)snprintf(message, message_capacity,
                    "reachable call target identity cannot be retained");
                return 0;
            }
            edge.expression = expression_id;
            (void)cm_vec_push(&state->edges, &edge);
            return 1;
        }
        if (message[0] == '\0') {
            (void)snprintf(message, message_capacity,
                "reachable call target is unsupported");
        }
        return 0;
    }
    }
    (void)snprintf(message, message_capacity,
        "reachable expression tree is malformed");
    return 0;
}

static int cm_compile_discover_reachable_hir(CmCompileExactState *state,
    char *message, size_t message_capacity)
{
    size_t instance_index;

    if (!cm_compile_exact_all_local_current(state)) {
        (void)snprintf(message, message_capacity,
            "whole-crate semantic admission became stale "
            "before reachability");
        return 0;
    }
    for (instance_index = 0u; instance_index < state->instances.len;
         ++instance_index) {
        CmCompileReachableInstance *instance;
        const CmHirItem *item;
        const CmHirBody *body;
        size_t edge_start;

        instance = (CmCompileReachableInstance *)cm_vec_at(
            &state->instances, instance_index);
        if (instance == NULL) return 0;
        item = cm_compile_definition_item(state->hir,
            instance->definition);
        body = item == NULL ? NULL
            : cm_hir_get_body(state->hir, instance->body);
        if (item == NULL || body == NULL
            || item->data.function_item.body != instance->body) {
            (void)snprintf(message, message_capacity,
                "reachable function has no HIR body");
            return 0;
        }
        if (body == NULL || body->state != CM_HIR_BODY_TYPED
            || !cm_compile_all_local_body_admitted(state,
                instance->body)) {
            (void)snprintf(message, message_capacity,
                "reachable function body lacks whole-crate semantic evidence");
            return 0;
        }
        if (body->root_expression == CM_HIR_EXPR_NONE) {
            (void)snprintf(message, message_capacity,
                "reachable function has no typed root expression");
            return 0;
        }
        edge_start = state->edges.len;
        if (!cm_compile_discover_expression_callees(state, instance_index,
                body->root_expression, 0u, message, message_capacity)) {
            return 0;
        }
        instance = (CmCompileReachableInstance *)cm_vec_at(
            &state->instances, instance_index);
        if (instance == NULL) return 0;
        instance->edge_start = edge_start;
        instance->edge_count = state->edges.len - edge_start;
    }
    if (!cm_compile_exact_all_local_current(state)) {
        (void)snprintf(message, message_capacity,
            "reachability invalidated whole-crate semantic admission");
        return 0;
    }
    return 1;
}

static int cm_compile_admit_instance_closure(CmCompileExactState *state,
    CmHirCrateId local_crate, CmSemanticAdmission *admission,
    char *message, size_t message_capacity)
{
    CmSemanticCanonicalReachableInstance *reachable;
    CmSemanticCanonicalReachableInstanceCall *calls;
    CmSemanticAdmissionResult admission_result;
    size_t instance_index;
    size_t edge_index;
    int ok;

    if (!cm_compile_exact_all_local_current(state)) {
        (void)snprintf(message, message_capacity,
            "whole-crate semantic admission became stale "
            "before exact admission");
        return 0;
    }
    if (state->instances.len == 0u
        || state->instances.len > (size_t)-1 / sizeof(*reachable)
        || state->edges.len > (size_t)-1 / sizeof(*calls)) return 0;
    reachable = (CmSemanticCanonicalReachableInstance *)cm_alloc_zeroed(
        state->instances.len, sizeof(*reachable));
    calls = state->edges.len == 0u ? NULL
        : (CmSemanticCanonicalReachableInstanceCall *)cm_alloc_zeroed(
            state->edges.len, sizeof(*calls));
    ok = 0;
    for (instance_index = 0u; instance_index < state->instances.len;
         ++instance_index) {
        const CmCompileReachableInstance *instance;

        instance = (const CmCompileReachableInstance *)cm_vec_at_const(
            &state->instances, instance_index);
        if (instance == NULL
            || cm_hir_canonical_instance_validate(state->hir, local_crate,
                &instance->identity) != CM_HIR_INSTANCE_OK
            || instance->identity.body != instance->body) goto cleanup;
        reachable[instance_index].identity = &instance->identity;
    }
    for (instance_index = 0u; instance_index < state->instances.len;
         ++instance_index) {
        const CmCompileReachableInstance *caller;
        size_t caller_edge;

        caller = (const CmCompileReachableInstance *)cm_vec_at_const(
            &state->instances, instance_index);
        if (caller == NULL || caller->edge_start > state->edges.len
            || caller->edge_count
                > state->edges.len - caller->edge_start) goto cleanup;
        for (caller_edge = 0u; caller_edge < caller->edge_count;
             ++caller_edge) {
            const CmCompileReachableEdge *edge;
            const CmHirExpr *expression;
            int equal_target;
            const CmCompileReachableInstance *callee_instance;

            edge_index = caller->edge_start + caller_edge;
            edge = (const CmCompileReachableEdge *)cm_vec_at_const(
                &state->edges, edge_index);
            expression = edge == NULL ? NULL
                : cm_hir_get_expr(state->hir, edge->expression);
            if (edge == NULL || expression == NULL
                || edge->callee >= state->instances.len) {
                goto cleanup;
            }
            if (expression->kind == CM_HIR_EXPR_QUALIFIED_CALL
                || expression->kind == CM_HIR_EXPR_METHOD_CALL) {
                CmSemanticCallableSelectionView selection;

                memset(&selection, 0, sizeof(selection));
                if (!cm_compile_callable_selection(state, expression,
                        edge->expression, &selection, message,
                        message_capacity)) goto cleanup;
            } else if (expression->kind != CM_HIR_EXPR_CALL) {
                goto cleanup;
            }
            equal_target = 0;
            callee_instance =
                (const CmCompileReachableInstance *)cm_vec_at_const(
                    &state->instances, edge->callee);
            if (callee_instance == NULL
                || cm_hir_canonical_instance_validate(state->hir,
                    local_crate, &edge->callee_identity)
                    != CM_HIR_INSTANCE_OK
                || cm_hir_canonical_instance_equal(&edge->callee_identity,
                    &callee_instance->identity, &equal_target)
                    != CM_HIR_INSTANCE_OK
                || !equal_target) {
                (void)snprintf(message, message_capacity,
                    "reachable call target identity is inconsistent");
                goto cleanup;
            }
            calls[edge_index].caller = &caller->identity;
            calls[edge_index].expression = edge->expression;
            calls[edge_index].callee = &callee_instance->identity;
        }
    }
    admission_result =
        cm_semantic_admit_typed_canonical_instance_closure(admission,
        state->hir, local_crate, reachable, state->instances.len, calls,
        state->edges.len);
    if (admission_result.status != CM_SEMANTIC_ADMISSION_OK) {
        if (admission_result.status == CM_SEMANTIC_ADMISSION_ITEM_FAILURE) {
            (void)snprintf(message, message_capacity, "%s",
                cm_semantic_item_status_name(
                    admission_result.item_result.status));
        } else if (admission_result.status
                == CM_SEMANTIC_ADMISSION_BODY_FAILURE) {
            (void)snprintf(message, message_capacity, "%s",
                cm_semantic_body_status_name(
                    admission_result.body_result.status));
        } else {
            (void)snprintf(message, message_capacity,
                "reachable semantic admission failed: %s",
                cm_semantic_admission_status_name(admission_result.status));
        }
        goto cleanup;
    }
    if (!cm_compile_exact_all_local_current(state)) {
        (void)snprintf(message, message_capacity,
            "exact admission invalidated whole-crate semantic admission");
        cm_semantic_admission_destroy(admission);
        goto cleanup;
    }
    state->admission = admission;
    ok = 1;

cleanup:
    cm_free(calls);
    cm_free(reachable);
    return ok;
}

static int cm_compile_publish_reachable_mir(CmCompileExactState *state,
    char *message, size_t message_capacity)
{
    CmMirPublication publication;
    CmMirStatus status;
    size_t index;
    int ok;

    if (!cm_compile_exact_all_local_current(state)) {
        (void)snprintf(message, message_capacity,
            "whole-crate semantic admission became stale before MIR");
        return 0;
    }
    cm_mir_publication_init(&publication);
    ok = 0;
    status = cm_mir_publication_begin(&publication, state->mir,
        state->admission);
    if (status != CM_MIR_OK) goto fail;
    for (index = 0u; index < state->instances.len; ++index) {
        CmCompileReachableInstance *instance;
        CmMirInstance canonical;
        const CmHirTypeId *substitutions;

        instance = (CmCompileReachableInstance *)cm_vec_at(
            &state->instances, index);
        if (instance == NULL) {
            status = CM_MIR_INVARIANT_VIOLATION;
            goto fail;
        }
        substitutions = instance->substitution_count == 0u
            ? NULL : &instance->substitution;
        memset(&canonical, 0, sizeof(canonical));
        canonical.definition = instance->identity.definition;
        canonical.substitutions = (CmHirTypeId *)substitutions;
        canonical.substitution_count = instance->substitution_count;
        canonical.body = instance->identity.body;
        canonical.identity_bytes = instance->identity.bytes;
        canonical.identity_size = instance->identity.size;
        status = cm_mir_publication_reserve_canonical(&publication,
            &canonical, instance->body, &instance->mir_body);
        if (status != CM_MIR_OK) goto fail;
    }
    for (index = 0u; index < state->instances.len; ++index) {
        CmCompileReachableInstance *instance;
        CmMirLowerResult result;

        instance = (CmCompileReachableInstance *)cm_vec_at(
            &state->instances, index);
        if (instance == NULL) {
            status = CM_MIR_INVARIANT_VIOLATION;
            goto fail;
        }
        result = cm_mir_lower_admitted_publication_canonical(state->mir,
            &publication, state->admission, instance->mir_body);
        if (result.error_count != 0u
            || result.body != instance->mir_body) {
            (void)snprintf(message, message_capacity, "%s",
                result.error_count != 0u
                    && result.first_error.message[0] != '\0'
                    ? result.first_error.message
                    : "reachable MIR lowering failed");
            goto fail;
        }
    }
    status = cm_mir_publication_validate(&publication);
    if (status != CM_MIR_OK) goto fail;
    if (!cm_compile_exact_all_local_current(state)) {
        (void)snprintf(message, message_capacity,
            "MIR construction invalidated whole-crate semantic admission");
        status = CM_MIR_INVALID_ADMISSION;
        goto fail;
    }
    status = cm_mir_publication_commit(&publication);
    if (status != CM_MIR_OK) goto fail;
    ok = 1;
fail:
    if (!ok && message[0] == '\0') {
        (void)snprintf(message, message_capacity,
            "reachable MIR publication failed: %s",
            cm_mir_status_name(status));
    }
    if (!ok) {
        for (index = 0u; index < state->instances.len; ++index) {
            CmCompileReachableInstance *instance;

            instance = (CmCompileReachableInstance *)cm_vec_at(
                &state->instances, index);
            if (instance != NULL) instance->mir_body = CM_MIR_BODY_NONE;
        }
    }
    cm_mir_publication_destroy(&publication);
    return ok;
}

static CmCompileResult cm_compile_local_admission_failure(
    const CmSemanticAdmissionResult *admission)
{
    CmCompileResult result;

    if (admission == NULL) {
        return cm_compile_result(CM_COMPILE_SEMANTIC,
            "whole-crate semantic admission failed");
    }
    if (admission->status == CM_SEMANTIC_ADMISSION_LOCAL_BODIES_FAILURE) {
        result = cm_compile_result(CM_COMPILE_BODY,
            "whole-crate body lowering failed");
        (void)snprintf(result.message, sizeof(result.message),
            "whole-crate body lowering failed: %s",
            cm_hir_local_bodies_status_name(admission->local_bodies.status));
        if (admission->local_bodies.status
                == CM_HIR_LOCAL_BODIES_BODY_FAILURE) {
            (void)snprintf(result.message, sizeof(result.message),
                "whole-crate body lowering failed: %s",
                cm_hir_body_lower_status_name(
                    admission->local_bodies.body_result.status));
        }
        return result;
    }
    result = cm_compile_result(CM_COMPILE_SEMANTIC,
        "whole-crate semantic admission failed");
    if (admission->status == CM_SEMANTIC_ADMISSION_ITEM_FAILURE) {
        (void)snprintf(result.message, sizeof(result.message),
            "whole-crate semantic item checking failed: %s",
            cm_semantic_item_status_name(admission->item_result.status));
    } else if (admission->status == CM_SEMANTIC_ADMISSION_BODY_FAILURE) {
        (void)snprintf(result.message, sizeof(result.message),
            "whole-crate semantic body checking failed: %s "
            "(body=%u expression=%u callee=%u:%u)",
            cm_semantic_body_status_name(admission->body_result.status),
            (unsigned int)admission->body,
            (unsigned int)admission->body_result.expression,
            (unsigned int)admission->body_result.callee.crate_id,
            (unsigned int)admission->body_result.callee.index);
    } else if (admission->status
            == CM_SEMANTIC_ADMISSION_SESSION_FAILURE) {
        (void)snprintf(result.message, sizeof(result.message),
            "whole-crate semantic session failed: %s",
            cm_trait_solver_result_name(admission->session_status));
    } else {
        (void)snprintf(result.message, sizeof(result.message),
            "whole-crate semantic admission failed: %s",
            cm_semantic_admission_status_name(admission->status));
    }
    return result;
}

CmCompileResult cm_compile_emit_c(const char *input_path,
    const char *output_path, enum cm_edition edition,
    const CmTargetDesc *target)
{
    CmCompileResult result;
    CmSourceSet sources;
    CmSourceId root_source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap module_map;
    CmHirLowerOptions hir_options;
    CmHirLowerResult hir_result;
    CmHirItemId entry_id;
    const CmHirItem *entry;
    CmMirContext mir;
    CmMirLowerResult mir_result;
    const CmMirBody *mir_body;
    CmStrBuf c_output;
    CmStrBuf temporary_path;
    CmCEmitStatus emit_status;
    FILE *output;
    struct stat input_stat;
    struct stat output_stat;
    int temporary_fd;
    int temporary_exists;
    int write_failed;
    size_t written;
    size_t cleanup_index;
    CmVec exact_root_items;
    CmVec exact_root_instances;
    CmVec exact_root_bodies;
    CmCompileExactState exact_state;
    CmSemanticSession legacy_semantic;
    CmSemanticAdmission all_local_admission;
    CmSemanticAdmission reachable_admission;
    int use_legacy_entry;

    result = cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
        "invalid compile request");
    if (input_path == NULL || input_path[0] == '\0' || output_path == NULL
        || output_path[0] == '\0' || target == NULL
        || strcmp(input_path, output_path) == 0) {
        return result;
    }

    if (!cm_target_cfg_set(&cfg, target)) {
        return cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
            "target cannot produce an exact cfg environment");
    }
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_import_resolver_init(&imports);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&module_map);
    cm_mir_context_init(&mir);
    cm_str_buf_init(&c_output);
    cm_str_buf_init(&temporary_path);
    cm_vec_init(&exact_root_items, sizeof(CmHirItemId));
    cm_vec_init(&exact_root_instances, sizeof(size_t));
    cm_vec_init(&exact_root_bodies, sizeof(CmMirBodyId));
    memset(&exact_state, 0, sizeof(exact_state));
    memset(&legacy_semantic, 0, sizeof(legacy_semantic));
    memset(&all_local_admission, 0, sizeof(all_local_admission));
    memset(&reachable_admission, 0, sizeof(reachable_admission));
    cm_vec_init(&exact_state.instances,
        sizeof(CmCompileReachableInstance));
    cm_vec_init(&exact_state.edges, sizeof(CmCompileReachableEdge));
    output = NULL;
    temporary_fd = -1;
    temporary_exists = 0;

    if (cm_mir_context_set_pointer_bits(&mir, target->pointer_bits)
            != CM_MIR_OK) {
        result = cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
            "unsupported target pointer width");
        goto cleanup;
    }

    if (cm_source_load_file(&sources, input_path, &root_source)
        != CM_SOURCE_OK) {
        result = cm_compile_result(CM_COMPILE_SOURCE_IO,
            "cannot load input source");
        goto cleanup;
    }
    if (stat(input_path, &input_stat) != 0) {
        result = cm_compile_result(CM_COMPILE_SOURCE_IO,
            "cannot identify input source");
        goto cleanup;
    }
    if (stat(output_path, &output_stat) == 0
        && input_stat.st_dev == output_stat.st_dev
        && input_stat.st_ino == output_stat.st_ino) {
        result = cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
            "input and output paths identify the same file");
        goto cleanup;
    }
    cm_module_graph_options_init(&graph_options);
    graph_options.edition = edition;
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    if (graph_result.root == CM_MODULE_NONE
        || graph_result.revision == CM_MODULE_GRAPH_REVISION_NONE
        || graph_result.error_count != 0u) {
        result = cm_compile_result(CM_COMPILE_MODULE_GRAPH,
            "source parsing, cfg expansion, or module graph construction "
            "failed");
        goto cleanup;
    }
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    if (import_result.error_count != 0u
        || import_result.revision != graph_result.revision) {
        result = cm_compile_result(CM_COMPILE_IMPORTS,
            "local imports did not resolve");
        goto cleanup;
    }
    cm_hir_lower_options_init(&hir_options);
    hir_options.crate_name = "cmrustc_input";
    hir_options.source = root_source;
    if (!cm_compile_hir_edition(edition, &hir_options.edition)) {
        result = cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
            "unsupported Rust edition");
        goto cleanup;
    }
    hir_result = cm_hir_lower_module_graph(&hir, &graph,
        graph_result.revision, &imports, &module_map, &hir_options);
    if (hir_result.error_count != 0u
        || hir_result.crate_id == CM_HIR_CRATE_NONE
        || hir_result.root_module == CM_HIR_MODULE_NONE) {
        result = cm_compile_result(CM_COMPILE_HIR,
            hir_result.error_count == 0u
                ? "HIR lowering produced no crate"
                : hir_result.first_error.message);
        goto cleanup;
    }
    {
        CmSemanticAdmissionResult admission_result;

        admission_result = cm_semantic_admit_local_crate(
            &all_local_admission, &hir, hir_result.crate_id, &graph,
            graph_result.revision, &imports, &module_map);
        if (admission_result.status != CM_SEMANTIC_ADMISSION_OK) {
            result = cm_compile_local_admission_failure(&admission_result);
            goto cleanup;
        }
    }
    exact_state.hir = &hir;
    exact_state.mir = &mir;
    exact_state.all_local_admission = &all_local_admission;
    exact_state.local_crate = hir_result.crate_id;
    exact_state.all_local_generation =
        cm_semantic_admission_generation(&all_local_admission);
    exact_state.all_local_capability_id =
        cm_semantic_admission_capability_id(&all_local_admission);
    if (!cm_compile_exact_all_local_current(&exact_state)) {
        result = cm_compile_result(CM_COMPILE_SEMANTIC,
            "whole-crate semantic admission was not retained");
        goto cleanup;
    }
    /* Every cfg-active supported local definition is typed before roots. */
    entry_id = cm_compile_find_entry(&hir, hir_result.root_module);
    use_legacy_entry = hir.items.len == 1u
        && entry_id != CM_HIR_ITEM_NONE;
    if (use_legacy_entry) {
        entry = cm_hir_get_item(&hir, entry_id);
        if (entry == NULL
            || entry->data.function_item.body == CM_HIR_BODY_NONE) {
            result = cm_compile_result(CM_COMPILE_HIR,
                "crate has no source-backed root main entry");
            goto cleanup;
        }
        {
            CmSemanticItemResult item_result;

            item_result = cm_semantic_item_check_local_trait_impls(&hir,
                hir_result.crate_id);
            if (item_result.status != CM_SEMANTIC_ITEM_OK) {
                result = cm_compile_result(CM_COMPILE_SEMANTIC,
                    cm_semantic_item_status_name(item_result.status));
                goto cleanup;
            }
        }
        {
            CmSemanticSessionOptions semantic_options;
            CmTraitSolverResultKind semantic_status;
            CmSemanticBodyResult semantic_result;

            cm_semantic_session_options_init(&semantic_options);
            semantic_options.local_crate = hir_result.crate_id;
            semantic_options.exact_owner = entry->definition;
            semantic_status = cm_semantic_session_init(&legacy_semantic,
                &hir, &semantic_options);
            if (semantic_status != CM_TRAIT_SOLVER_PROVEN) {
                result = cm_compile_result(CM_COMPILE_SEMANTIC,
                    cm_trait_solver_result_name(semantic_status));
                goto cleanup;
            }
            semantic_result = cm_semantic_body_check_calls(
                &legacy_semantic, entry->data.function_item.body,
                NULL, 0u);
            if (semantic_result.status != CM_SEMANTIC_BODY_OK) {
                result = cm_compile_result(CM_COMPILE_SEMANTIC,
                    cm_semantic_body_status_name(semantic_result.status));
                goto cleanup;
            }
        }
        if (!cm_compile_exact_all_local_current(&exact_state)) {
            result = cm_compile_result(CM_COMPILE_SEMANTIC,
                "whole-crate semantic admission became stale before MIR");
            goto cleanup;
        }
        /* HIR and semantics are complete before the legacy MIR phase. */
        mir_result = cm_mir_lower_body(&mir, &hir,
            entry->data.function_item.body);
        if (mir_result.error_count != 0u
            || mir_result.body == CM_MIR_BODY_NONE) {
            result = cm_compile_result(CM_COMPILE_MIR,
                mir_result.error_count == 0u
                    ? "MIR lowering produced no body"
                    : mir_result.first_error.message);
            goto cleanup;
        }
        mir_body = cm_mir_get_body(&mir, mir_result.body);
        emit_status = cm_c_emit_program(&c_output, &hir, mir_body,
            entry_id, target);
    } else {
        size_t item_index;

        for (item_index = 0u; item_index < hir.items.len; ++item_index) {
            const CmHirItem *candidate;
            CmHirItemId candidate_id;

            candidate = (const CmHirItem *)cm_vec_at_const(&hir.items,
                item_index);
            candidate_id = (CmHirItemId)(item_index + 1u);
            if (cm_compile_is_export_root(&hir, candidate)) {
                (void)cm_vec_push(&exact_root_items, &candidate_id);
            }
        }
        if (exact_root_items.len == 0u
            || exact_root_items.len > (size_t)UINT32_MAX) {
            result = cm_compile_result(CM_COMPILE_HIR,
                "crate has no supported exported root");
            goto cleanup;
        }
        for (item_index = 0u; item_index < exact_root_items.len;
             ++item_index) {
            const CmHirItemId *root_item_id;
            const CmHirItem *root_item;
            size_t root_instance;
            char message[160];

            root_item_id = (const CmHirItemId *)cm_vec_at_const(
                &exact_root_items, item_index);
            root_item = root_item_id == NULL ? NULL
                : cm_hir_get_item(&hir, *root_item_id);
            memset(message, 0, sizeof(message));
            if (root_item == NULL || !cm_compile_intern_exact(&exact_state,
                    root_item, NULL, 0u, &root_instance, message,
                    sizeof(message))) {
                result = cm_compile_result(CM_COMPILE_BODY,
                    message[0] == '\0' ? "reachable root lowering failed"
                        : message);
                goto cleanup;
            }
            (void)cm_vec_push(&exact_root_instances, &root_instance);
        }
        {
            char message[160];

            memset(message, 0, sizeof(message));
            if (!cm_compile_discover_reachable_hir(&exact_state, message,
                    sizeof(message))) {
                result = cm_compile_result(CM_COMPILE_BODY,
                    message[0] == '\0' ? "reachable discovery failed"
                        : message);
                goto cleanup;
            }
        }
        /* Reachability only reads HIR admitted by the whole-crate barrier. */
        {
            char message[160];

            memset(message, 0, sizeof(message));
            if (!cm_compile_admit_instance_closure(&exact_state,
                    hir_result.crate_id, &reachable_admission, message,
                    sizeof(message))) {
                result = cm_compile_result(CM_COMPILE_SEMANTIC,
                    message[0] == '\0'
                        ? "reachable semantic admission failed"
                        : message);
                goto cleanup;
            }
        }
        /* Every reachable call obligation is proven before any MIR exists. */
        {
            char message[160];

            memset(message, 0, sizeof(message));
            if (!cm_compile_publish_reachable_mir(&exact_state, message,
                    sizeof(message))) {
                result = cm_compile_result(CM_COMPILE_MIR,
                    message[0] == '\0'
                        ? "reachable MIR publication failed" : message);
                goto cleanup;
            }
        }
        for (item_index = 0u; item_index < exact_root_instances.len;
             ++item_index) {
            const size_t *root_instance;
            CmMirBodyId root_body;
            const CmCompileReachableInstance *instance;

            root_instance = (const size_t *)cm_vec_at_const(
                &exact_root_instances, item_index);
            instance = root_instance == NULL ? NULL
                : (const CmCompileReachableInstance *)cm_vec_at_const(
                    &exact_state.instances, *root_instance);
            if (instance == NULL || instance->mir_body == CM_MIR_BODY_NONE) {
                result = cm_compile_result(CM_COMPILE_MIR,
                    "reachable root has no published MIR body");
                goto cleanup;
            }
            root_body = instance->mir_body;
            (void)cm_vec_push(&exact_root_bodies, &root_body);
        }
        emit_status = cm_c_emit_reachable_program(&c_output, &hir, &mir,
            &reachable_admission,
            (const CmMirBodyId *)exact_root_bodies.data,
            (uint32_t)exact_root_bodies.len, target);
    }
    if (emit_status != CM_C_EMIT_OK || c_output.len == 0u) {
        result = cm_compile_result(CM_COMPILE_CODEGEN,
            cm_c_emit_status_name(emit_status));
        goto cleanup;
    }

    cm_str_buf_append(&temporary_path, output_path);
    cm_str_buf_append(&temporary_path, ".tmp.XXXXXX");
    temporary_fd = cm_compile_open_temporary(temporary_path.data);
    if (temporary_fd < 0) {
        result = cm_compile_result(CM_COMPILE_OUTPUT_IO,
            "cannot create temporary C output");
        goto cleanup;
    }
    temporary_exists = 1;
    output = fdopen(temporary_fd, "wb");
    if (output == NULL) {
        (void)close(temporary_fd);
        temporary_fd = -1;
        result = cm_compile_result(CM_COMPILE_OUTPUT_IO,
            "cannot open temporary C output stream");
        goto cleanup;
    }
    temporary_fd = -1;
    written = fwrite(c_output.data, 1u, c_output.len, output);
    write_failed = written != c_output.len;
    if (fflush(output) != 0) write_failed = 1;
    if (fclose(output) != 0) write_failed = 1;
    output = NULL;
    if (write_failed) {
        result = cm_compile_result(CM_COMPILE_OUTPUT_IO,
            "cannot write complete C output");
        goto cleanup;
    }
    if (rename(temporary_path.data, output_path) != 0) {
        result = cm_compile_result(CM_COMPILE_OUTPUT_IO,
            "cannot publish complete C output");
        goto cleanup;
    }
    temporary_exists = 0;
    result = cm_compile_result(CM_COMPILE_OK, "ok");

cleanup:
    if (output != NULL) (void)fclose(output);
    if (temporary_fd >= 0) (void)close(temporary_fd);
    if (temporary_exists) (void)remove(temporary_path.data);
    cm_str_buf_destroy(&temporary_path);
    cm_str_buf_destroy(&c_output);
    cm_semantic_session_destroy(&legacy_semantic);
    for (cleanup_index = 0u; cleanup_index < exact_state.edges.len;
         ++cleanup_index) {
        CmCompileReachableEdge *edge;

        edge = (CmCompileReachableEdge *)cm_vec_at(&exact_state.edges,
            cleanup_index);
        if (edge != NULL) {
            cm_hir_canonical_instance_destroy(&edge->callee_identity);
        }
    }
    for (cleanup_index = 0u; cleanup_index < exact_state.instances.len;
         ++cleanup_index) {
        CmCompileReachableInstance *instance;

        instance = (CmCompileReachableInstance *)cm_vec_at(
            &exact_state.instances, cleanup_index);
        if (instance != NULL) {
            cm_hir_canonical_instance_destroy(&instance->identity);
        }
    }
    cm_vec_destroy(&exact_state.edges);
    cm_vec_destroy(&exact_state.instances);
    cm_vec_destroy(&exact_root_bodies);
    cm_vec_destroy(&exact_root_instances);
    cm_vec_destroy(&exact_root_items);
    cm_mir_context_destroy(&mir);
    cm_semantic_admission_destroy(&reachable_admission);
    cm_semantic_admission_destroy(&all_local_admission);
    cm_hir_module_map_destroy(&module_map);
    cm_hir_context_destroy(&hir);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return result;
}

CmCompileResult cm_compile_emit_cmhir_kind(const char *input_path,
    const char *output_path, const char *crate_name,
    enum cm_edition edition, const CmTargetDesc *target,
    const CmCompileCmhirDependency *dependencies,
    size_t dependency_count, enum CmCompileCmhirKind output_kind)
{
    CmCompileResult result;
    CmSourceSet sources;
    CmSourceId root_source;
    CmCfgSet cfg;
    CmModuleGraph graph;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirContext hir;
    CmHirModuleMap module_map;
    CmHirLowerOptions hir_options;
    CmHirLowerResult hir_result;
    CmHirLibraryArtifact output_artifact;
    CmHirLibraryArtifactResult library_result;
    CmHirMetadataArtifactResult metadata_result;
    CmHirLibraryArtifact *dependency_artifacts;
    const CmHirLibraryArtifact **dependency_views;
    size_t initialized_dependencies;
    CmByteBuf encoded;
    CmStrBuf temporary_path;
    FILE *output;
    struct stat input_stat;
    struct stat output_stat;
    int output_exists;
    int temporary_fd;
    int temporary_exists;
    int write_failed;
    size_t written;
    size_t dependency_index;

    result = cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
        "invalid cmhir emission request");
    if (input_path == NULL || input_path[0] == '\0'
        || output_path == NULL || output_path[0] == '\0'
        || !cm_compile_identifier_valid(crate_name) || target == NULL
        || (output_kind != CM_COMPILE_CMHIR_DECLARATION
            && output_kind != CM_COMPILE_CMHIR_SEMANTIC)
        || strcmp(input_path, output_path) == 0
        || (dependency_count != 0u && dependencies == NULL)
        || dependency_count > (size_t)UINT32_MAX
        || dependency_count > (size_t)-1 / sizeof(*dependency_artifacts)
        || dependency_count > (size_t)-1 / sizeof(*dependency_views)) {
        return result;
    }
    for (dependency_index = 0u; dependency_index < dependency_count;
            ++dependency_index) {
        size_t prior_index;

        if (!cm_compile_identifier_valid(
                dependencies[dependency_index].extern_name)
            || dependencies[dependency_index].path == NULL
            || dependencies[dependency_index].path[0] == '\0'
            || (dependencies[dependency_index].kind
                    != CM_COMPILE_CMHIR_DECLARATION
                && dependencies[dependency_index].kind
                    != CM_COMPILE_CMHIR_SEMANTIC)
            || strcmp(output_path,
                dependencies[dependency_index].path) == 0) return result;
        for (prior_index = 0u; prior_index < dependency_index;
                ++prior_index) {
            if (strcmp(dependencies[dependency_index].extern_name,
                    dependencies[prior_index].extern_name) == 0) {
                return cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
                    "duplicate cmhir dependency extern name");
            }
        }
    }
    if (!cm_target_cfg_set(&cfg, target)) {
        return cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
            "target cannot produce an exact cfg environment");
    }

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_import_resolver_init(&imports);
    cm_hir_context_init(&hir);
    cm_hir_module_map_init(&module_map);
    cm_hir_library_artifact_init(&output_artifact);
    cm_byte_buf_init(&encoded);
    cm_str_buf_init(&temporary_path);
    dependency_artifacts = NULL;
    dependency_views = NULL;
    initialized_dependencies = 0u;
    output = NULL;
    output_exists = 0;
    temporary_fd = -1;
    temporary_exists = 0;

    if (cm_source_load_file(&sources, input_path, &root_source)
            != CM_SOURCE_OK) {
        result = cm_compile_result(CM_COMPILE_SOURCE_IO,
            "cannot load input source");
        goto cleanup_cmhir;
    }
    if (stat(input_path, &input_stat) != 0) {
        result = cm_compile_result(CM_COMPILE_SOURCE_IO,
            "cannot identify input source");
        goto cleanup_cmhir;
    }
    if (stat(output_path, &output_stat) == 0) {
        output_exists = 1;
        if (cm_compile_same_file(&input_stat, &output_stat)) {
            result = cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
                "input and output paths identify the same file");
            goto cleanup_cmhir;
        }
    }

    if (dependency_count != 0u) {
        dependency_artifacts = (CmHirLibraryArtifact *)calloc(
            dependency_count, sizeof(*dependency_artifacts));
        dependency_views = (const CmHirLibraryArtifact **)calloc(
            dependency_count, sizeof(*dependency_views));
        if (dependency_artifacts == NULL || dependency_views == NULL) {
            result = cm_compile_result(CM_COMPILE_METADATA,
                "cannot allocate cmhir dependency state");
            goto cleanup_cmhir;
        }
    }
    for (dependency_index = 0u; dependency_index < dependency_count;
            ++dependency_index) {
        CmSourceId metadata_source;
        const CmSourceFile *metadata_file;
        struct stat dependency_stat;

        cm_hir_library_artifact_init(
            &dependency_artifacts[dependency_index]);
        initialized_dependencies += 1u;
        if (cm_source_load_file_bounded(&sources,
                dependencies[dependency_index].path,
                (size_t)UINT32_C(67108864), &metadata_source)
                != CM_SOURCE_OK) {
            result = cm_compile_result(CM_COMPILE_SOURCE_IO,
                "cannot load cmhir dependency");
            goto cleanup_cmhir;
        }
        metadata_file = cm_source_get(&sources, metadata_source);
        if (metadata_file == NULL
            || stat(dependencies[dependency_index].path,
                &dependency_stat) != 0) {
            result = cm_compile_result(CM_COMPILE_SOURCE_IO,
                "cannot identify cmhir dependency");
            goto cleanup_cmhir;
        }
        if (output_exists
            && cm_compile_same_file(&output_stat, &dependency_stat)) {
            result = cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
                "dependency and output paths identify the same file");
            goto cleanup_cmhir;
        }
        if (dependencies[dependency_index].kind
                == CM_COMPILE_CMHIR_SEMANTIC) {
            metadata_result = cm_hir_metadata_decode_semantic_artifact(&hir,
                &dependency_artifacts[dependency_index],
                metadata_file->bytes, metadata_file->length,
                dependencies[dependency_index].extern_name,
                metadata_source);
        } else if (dependencies[dependency_index].kind
                == CM_COMPILE_CMHIR_DECLARATION) {
            metadata_result = cm_hir_metadata_decode_artifact(&hir,
                &dependency_artifacts[dependency_index],
                metadata_file->bytes, metadata_file->length,
                dependencies[dependency_index].extern_name,
                metadata_source);
        } else {
            result = cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
                "invalid cmhir dependency kind");
            goto cleanup_cmhir;
        }
        if (metadata_result.status != CM_HIR_METADATA_ARTIFACT_OK) {
            result = cm_compile_result(CM_COMPILE_METADATA,
                "cmhir dependency decode failed");
            (void)snprintf(result.message, sizeof(result.message),
                "cmhir dependency '%s' decode failed: %s",
                dependencies[dependency_index].extern_name,
                cm_hir_metadata_artifact_status_name(
                    metadata_result.status));
            goto cleanup_cmhir;
        }
        dependency_views[dependency_index] =
            &dependency_artifacts[dependency_index];
    }

    cm_module_graph_options_init(&graph_options);
    graph_options.edition = edition;
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    if (graph_result.root == CM_MODULE_NONE
        || graph_result.revision == CM_MODULE_GRAPH_REVISION_NONE
        || graph_result.error_count != 0u) {
        result = cm_compile_result(CM_COMPILE_MODULE_GRAPH,
            "source parsing, cfg expansion, or module graph construction "
            "failed");
        goto cleanup_cmhir;
    }
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    if (import_result.revision != graph_result.revision) {
        result = cm_compile_result(CM_COMPILE_IMPORTS,
            "import resolution did not publish the graph revision");
        goto cleanup_cmhir;
    }
    cm_hir_lower_options_init(&hir_options);
    hir_options.crate_name = crate_name;
    hir_options.source = root_source;
    hir_options.dependency_libraries = dependency_views;
    hir_options.dependency_library_count = dependency_count;
    if (!cm_compile_hir_edition(edition, &hir_options.edition)) {
        result = cm_compile_result(CM_COMPILE_INVALID_ARGUMENT,
            "unsupported Rust edition");
        goto cleanup_cmhir;
    }
    hir_result = cm_hir_lower_module_graph(&hir, &graph,
        graph_result.revision, &imports, &module_map, &hir_options);
    if (hir_result.error_count != 0u
        || hir_result.crate_id == CM_HIR_CRATE_NONE
        || hir_result.root_module == CM_HIR_MODULE_NONE) {
        result = cm_compile_result(CM_COMPILE_HIR,
            hir_result.error_count == 0u
                ? "HIR lowering produced no crate"
                : hir_result.first_error.message);
        goto cleanup_cmhir;
    }
    library_result = cm_hir_library_artifact_build(&output_artifact, &hir,
        hir_result.crate_id, &graph, graph_result.revision, &module_map,
        crate_name);
    if (library_result.status != CM_HIR_LIBRARY_OK) {
        result = cm_compile_result(CM_COMPILE_METADATA,
            "cmhir declaration capture failed");
        (void)snprintf(result.message, sizeof(result.message),
            "cmhir declaration capture failed: %s",
            cm_hir_library_status_name(library_result.status));
        goto cleanup_cmhir;
    }
    metadata_result = output_kind == CM_COMPILE_CMHIR_SEMANTIC
        ? cm_hir_metadata_encode_semantic_artifact(&encoded,
            &output_artifact)
        : cm_hir_metadata_encode_artifact(&encoded, &output_artifact);
    if (metadata_result.status != CM_HIR_METADATA_ARTIFACT_OK
        || encoded.len == 0u) {
        result = cm_compile_result(CM_COMPILE_METADATA,
            "cmhir metadata encoding failed");
        (void)snprintf(result.message, sizeof(result.message),
            "cmhir metadata encoding failed: %s",
            cm_hir_metadata_artifact_status_name(metadata_result.status));
        goto cleanup_cmhir;
    }

    cm_str_buf_append(&temporary_path, output_path);
    cm_str_buf_append(&temporary_path, ".tmp.XXXXXX");
    temporary_fd = cm_compile_open_temporary(temporary_path.data);
    if (temporary_fd < 0) {
        result = cm_compile_result(CM_COMPILE_OUTPUT_IO,
            "cannot create temporary cmhir output");
        goto cleanup_cmhir;
    }
    temporary_exists = 1;
    output = fdopen(temporary_fd, "wb");
    if (output == NULL) {
        (void)close(temporary_fd);
        temporary_fd = -1;
        result = cm_compile_result(CM_COMPILE_OUTPUT_IO,
            "cannot open temporary cmhir output stream");
        goto cleanup_cmhir;
    }
    temporary_fd = -1;
    written = fwrite(encoded.data, 1u, encoded.len, output);
    write_failed = written != encoded.len;
    if (fflush(output) != 0) write_failed = 1;
    if (fclose(output) != 0) write_failed = 1;
    output = NULL;
    if (write_failed) {
        result = cm_compile_result(CM_COMPILE_OUTPUT_IO,
            "cannot write complete cmhir output");
        goto cleanup_cmhir;
    }
    if (rename(temporary_path.data, output_path) != 0) {
        result = cm_compile_result(CM_COMPILE_OUTPUT_IO,
            "cannot publish complete cmhir output");
        goto cleanup_cmhir;
    }
    temporary_exists = 0;
    result = cm_compile_result(CM_COMPILE_OK, "ok");

cleanup_cmhir:
    if (output != NULL) (void)fclose(output);
    if (temporary_fd >= 0) (void)close(temporary_fd);
    if (temporary_exists) (void)remove(temporary_path.data);
    cm_str_buf_destroy(&temporary_path);
    cm_byte_buf_destroy(&encoded);
    cm_hir_library_artifact_destroy(&output_artifact);
    for (dependency_index = 0u;
            dependency_index < initialized_dependencies;
            ++dependency_index) {
        cm_hir_library_artifact_destroy(
            &dependency_artifacts[dependency_index]);
    }
    free(dependency_views);
    free(dependency_artifacts);
    cm_hir_module_map_destroy(&module_map);
    cm_hir_context_destroy(&hir);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
    return result;
}

CmCompileResult cm_compile_emit_cmhir(const char *input_path,
    const char *output_path, const char *crate_name,
    enum cm_edition edition, const CmTargetDesc *target,
    const CmCompileCmhirDependency *dependencies,
    size_t dependency_count)
{
    return cm_compile_emit_cmhir_kind(input_path, output_path, crate_name,
        edition, target, dependencies, dependency_count,
        CM_COMPILE_CMHIR_DECLARATION);
}

const char *cm_compile_status_name(CmCompileStatus status)
{
    switch (status) {
    case CM_COMPILE_OK: return "ok";
    case CM_COMPILE_INVALID_ARGUMENT: return "invalid argument";
    case CM_COMPILE_SOURCE_IO: return "source I/O";
    case CM_COMPILE_MODULE_GRAPH: return "module graph";
    case CM_COMPILE_IMPORTS: return "imports";
    case CM_COMPILE_HIR: return "HIR";
    case CM_COMPILE_BODY: return "body HIR";
    case CM_COMPILE_SEMANTIC: return "semantic";
    case CM_COMPILE_MIR: return "MIR";
    case CM_COMPILE_CODEGEN: return "C code generation";
    case CM_COMPILE_METADATA: return "cmhir metadata";
    case CM_COMPILE_OUTPUT_IO: return "output I/O";
    }
    return "unknown compile status";
}
