#include "cm/hir/declaration_metadata.h"

#include "cm/alloc.h"
#include "metadata_codec.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define CM_DECL_SECTION_COUNT 12u
#define CM_DECL_FAMILY_COUNT 14u
#define CM_DECL_STRING_MAX ((size_t)1048576u)
#define CM_DECL_DEPTH_MAX 1024u

typedef struct CmDeclFamily {
    uint8_t state;
    uint32_t count;
    uint32_t crc;
} CmDeclFamily;

static const unsigned char cm_decl_tags[CM_DECL_SECTION_COUNT][4] = {
    { 'C', 'R', 'A', 'T' }, { 'M', 'A', 'N', 'F' },
    { 'M', 'O', 'D', 'S' }, { 'N', 'O', 'M', 'D' },
    { 'A', 'I', 'T', 'M' }, { 'G', 'P', 'A', 'R' },
    { 'T', 'Y', 'P', 'E' }, { 'I', 'T', 'E', 'M' },
    { 'V', 'A', 'L', 'U' }, { 'P', 'R', 'E', 'D' },
    { 'I', 'M', 'P', 'L' }, { 'N', 'S', 'P', 'C' }
};

static int cm_decl_count_valid(size_t count, size_t maximum)
{
    return count <= maximum
        && count <= (size_t)UINT32_MAX;
}

static int cm_decl_bytes_valid(CmHirDeclarationString value,
    size_t minimum, size_t maximum)
{
    size_t index;
    if (value.length < minimum || value.length > maximum
        || (value.length != 0u && value.data == NULL)) return 0;
    for (index = 0u; index < value.length; ++index) {
        if (value.data[index] == 0u) return 0;
    }
    return 1;
}

static int cm_decl_identifier(CmHirDeclarationString value)
{
    size_t index;
    unsigned char byte;
    if (!cm_decl_bytes_valid(value, 1u, CM_DECL_STRING_MAX)) return 0;
    byte = value.data[0];
    if (!((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
        || (byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
        || byte == (unsigned char)'_')) return 0;
    for (index = 1u; index < value.length; ++index) {
        byte = value.data[index];
        if (!((byte >= (unsigned char)'A' && byte <= (unsigned char)'Z')
            || (byte >= (unsigned char)'a' && byte <= (unsigned char)'z')
            || (byte >= (unsigned char)'0' && byte <= (unsigned char)'9')
            || byte == (unsigned char)'_')) return 0;
    }
    return 1;
}

static int cm_decl_string_compare(CmHirDeclarationString left,
    CmHirDeclarationString right)
{
    size_t common;
    int compared;
    common = left.length < right.length ? left.length : right.length;
    compared = common == 0u ? 0 : memcmp(left.data, right.data, common);
    if (compared != 0) return compared;
    return left.length < right.length ? -1 : left.length > right.length;
}

static int cm_decl_string_equal(CmHirDeclarationString left,
    CmHirDeclarationString right)
{
    return left.length == right.length
        && (left.length == 0u
            || memcmp(left.data, right.data, left.length) == 0);
}

static int cm_decl_string_is(CmHirDeclarationString value,
    const char *literal);

static int cm_decl_primitive(uint8_t value)
{
    return value >= (uint8_t)CM_HIR_DECL_PRIMITIVE_UNIT
        && value <= (uint8_t)CM_HIR_DECL_PRIMITIVE_F64;
}

static int cm_decl_namespace_primitive(uint32_t value)
{
    /* `()` has no identifier namespace binding.  All remaining declaration
     * primitive tags map exactly to resolver primitive kinds. */
    return value >= (uint32_t)CM_HIR_DECL_PRIMITIVE_BOOL
        && value <= (uint32_t)CM_HIR_DECL_PRIMITIVE_F64;
}

static int cm_decl_range(uint32_t start, uint32_t count, size_t total)
{
    size_t first;
    if (count == 0u) return start == 0u;
    if (start == 0u) return 0;
    first = (size_t)start - 1u;
    return first <= total && (size_t)count <= total - first;
}

static int cm_decl_type_local(const CmHirDeclarationMetadata *metadata,
    uint32_t local)
{
    return local != 0u && (size_t)local <= metadata->type_count;
}

static int cm_decl_generic_local(const CmHirDeclarationMetadata *metadata,
    uint32_t local)
{
    return local != 0u && (size_t)local <= metadata->generic_count;
}

void cm_hir_declaration_metadata_init(CmHirDeclarationMetadata *metadata)
{
    if (metadata != NULL) memset(metadata, 0, sizeof(*metadata));
}

static void cm_decl_free_string(CmHirDeclarationString *value)
{
    cm_free(value->data);
    value->data = NULL;
    value->length = 0u;
}

void cm_hir_declaration_metadata_destroy(CmHirDeclarationMetadata *metadata)
{
    size_t index;
    if (metadata == NULL) return;
    if (!metadata->owns_storage) {
        memset(metadata, 0, sizeof(*metadata));
        return;
    }
    cm_decl_free_string(&metadata->crate_name);
    cm_decl_free_string(&metadata->crate_disambiguator);
    cm_decl_free_string(&metadata->target_triple);
    cm_decl_free_string(&metadata->data_layout);
    for (index = 0u; index < metadata->cfg_count; ++index)
        cm_decl_free_string(&metadata->cfgs[index]);
    for (index = 0u; index < metadata->module_count; ++index)
        cm_decl_free_string(&metadata->modules[index].name);
    for (index = 0u; index < metadata->trait_count; ++index) {
        uint32_t child;
        cm_decl_free_string(&metadata->traits[index].name);
        cm_decl_free_string(&metadata->traits[index].diagnostic_item);
        cm_decl_free_string(&metadata->traits[index].lang_item);
        for (child = 0u; child < metadata->traits[index].supertrait_count;
                ++child)
            cm_free(metadata->traits[index].supertraits[child]
                .argument_types);
        cm_free(metadata->traits[index].supertraits);
    }
    for (index = 0u; index < metadata->associated_count; ++index) {
        cm_decl_free_string(&metadata->associated_items[index].name);
        cm_decl_free_string(&metadata->associated_items[index].abi);
        cm_decl_free_string(&metadata->associated_items[index].lang_item);
        cm_free(metadata->associated_items[index].parameter_types);
    }
    for (index = 0u; index < metadata->generic_count; ++index)
        cm_decl_free_string(&metadata->generics[index].name);
    for (index = 0u; index < metadata->type_count; ++index) {
        cm_free(metadata->types[index].argument_types);
        cm_free(metadata->types[index].element_types);
        cm_free(metadata->types[index].projection_argument_types);
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        uint32_t child;
        cm_decl_free_string(&metadata->items[index].name);
        cm_decl_free_string(&metadata->items[index].lang_item);
        for (child = 0u; metadata->items[index].fields != NULL
                && child < metadata->items[index].field_count; ++child)
            cm_decl_free_string(&metadata->items[index].fields[child].name);
        cm_free(metadata->items[index].fields);
        cm_decl_free_string(&metadata->items[index].diagnostic_item);
        cm_decl_free_string(&metadata->items[index].enum_lang_item);
        for (child = 0u; metadata->items[index].variants != NULL
                && child < metadata->items[index].variant_count;
                ++child) {
            cm_decl_free_string(&metadata->items[index].variants[child].name);
            cm_decl_free_string(
                &metadata->items[index].variants[child].lang_item);
            cm_free(metadata->items[index].variants[child].fields);
        }
        cm_free(metadata->items[index].variants);
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        cm_decl_free_string(&metadata->values[index].name);
        cm_free(metadata->values[index].parameter_types);
    }
    for (index = 0u; index < metadata->predicate_count; ++index) {
        cm_free(metadata->predicates[index].argument_types);
        cm_free(metadata->predicates[index].equalities);
    }
    for (index = 0u; index < metadata->namespace_count; ++index)
        cm_decl_free_string(&metadata->namespace_entries[index].name);
    cm_free(metadata->cfgs);
    cm_free(metadata->modules);
    cm_free(metadata->traits);
    cm_free(metadata->associated_items);
    cm_free(metadata->generics);
    cm_free(metadata->types);
    cm_free(metadata->items);
    cm_free(metadata->values);
    cm_free(metadata->predicates);
    cm_free(metadata->outlives_predicates);
    cm_free(metadata->namespace_entries);
    memset(metadata, 0, sizeof(*metadata));
}

static int cm_decl_module_path_compare(
    const CmHirDeclarationMetadata *metadata, size_t left, size_t right,
    int *valid)
{
    uint32_t left_chain[CM_DECL_DEPTH_MAX];
    uint32_t right_chain[CM_DECL_DEPTH_MAX];
    size_t left_count;
    size_t right_count;
    size_t index;
    left_count = 0u;
    right_count = 0u;
    while (left != 0u) {
        if (left_count == CM_DECL_DEPTH_MAX
            || left >= metadata->module_count) {
            *valid = 0;
            return 0;
        }
        left_chain[left_count++] = (uint32_t)left;
        left = (size_t)metadata->modules[left].parent_module;
        if (left != 0u) left -= 1u;
    }
    while (right != 0u) {
        if (right_count == CM_DECL_DEPTH_MAX
            || right >= metadata->module_count) {
            *valid = 0;
            return 0;
        }
        right_chain[right_count++] = (uint32_t)right;
        right = (size_t)metadata->modules[right].parent_module;
        if (right != 0u) right -= 1u;
    }
    for (index = 0u; index < left_count && index < right_count; ++index) {
        int order;
        order = cm_decl_string_compare(
            metadata->modules[left_chain[left_count - index - 1u]].name,
            metadata->modules[right_chain[right_count - index - 1u]].name);
        if (order != 0) return order;
    }
    return left_count < right_count ? -1 : left_count > right_count;
}

static int cm_decl_validate_modules(const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t root_count;
    int valid;
    if (metadata->module_count == 0u || metadata->modules == NULL
        || metadata->root_module == 0u
        || (size_t)metadata->root_module > metadata->module_count) return 0;
    root_count = 0u;
    valid = 1;
    for (index = 0u; index < metadata->module_count; ++index) {
        const CmHirDeclarationModule *module;
        module = &metadata->modules[index];
        if (!cm_decl_identifier(module->name)) return 0;
        if (module->parent_module == 0u) {
            root_count += 1u;
            if (index + 1u != (size_t)metadata->root_module) return 0;
        } else if ((size_t)module->parent_module > index) {
            return 0;
        }
        if (index != 0u
            && cm_decl_module_path_compare(metadata, index - 1u, index,
                &valid) >= 0) return 0;
        if (!valid) return 0;
    }
    return root_count == 1u;
}

static int cm_decl_visibility_valid(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationVisibility *visibility, uint32_t owner_module)
{
    uint32_t module;
    if (visibility->kind == CM_HIR_DECL_VISIBILITY_PRIVATE
        || visibility->kind == CM_HIR_DECL_VISIBILITY_PUBLIC
        || visibility->kind == CM_HIR_DECL_VISIBILITY_CRATE)
        return visibility->restriction_module == 0u;
    if (visibility->kind != CM_HIR_DECL_VISIBILITY_RESTRICTED
        || visibility->restriction_module == 0u
        || visibility->restriction_module == metadata->root_module
        || (size_t)visibility->restriction_module > metadata->module_count
        || owner_module == 0u
        || (size_t)owner_module > metadata->module_count)
        return 0;
    /* A restricted visibility names a proper ancestor; self is PRIVATE. */
    module = metadata->modules[owner_module - 1u].parent_module;
    while (module != 0u) {
        if (module == visibility->restriction_module) return 1;
        module = metadata->modules[module - 1u].parent_module;
    }
    return 0;
}

static int cm_decl_validate_generics(
    const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t owner_index;
    unsigned char *seen;
    if ((metadata->generic_count != 0u && metadata->generics == NULL)
        || (metadata->type_count != 0u && metadata->types == NULL)) return 0;
    seen = metadata->generic_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->generic_count,
            sizeof(unsigned char));
    for (index = 0u; index < metadata->generic_count; ++index) {
        const CmHirDeclarationGeneric *generic;
        generic = &metadata->generics[index];
        if ((generic->owner_kind != CM_HIR_DECL_GENERIC_NOMINAL
                && generic->owner_kind != CM_HIR_DECL_GENERIC_ITEM
                && generic->owner_kind != CM_HIR_DECL_GENERIC_VALUE)
            || generic->owner_local == 0u
            || (generic->kind != CM_HIR_DECL_GENERIC_TYPE
                && generic->kind != CM_HIR_DECL_GENERIC_CONST)
            || generic->is_relaxed_sized > 1u
            || generic->has_default != 0u
            || !cm_decl_identifier(generic->name)) {
            cm_free(seen);
            return 0;
        }
        if (generic->kind == CM_HIR_DECL_GENERIC_CONST) {
            const CmHirDeclarationType *declared;
            if ((generic->owner_kind != CM_HIR_DECL_GENERIC_ITEM
                    && generic->owner_kind != CM_HIR_DECL_GENERIC_VALUE)
                || generic->is_relaxed_sized != 0u
                || !cm_decl_type_local(metadata, generic->declared_type)) {
                cm_free(seen);
                return 0;
            }
            declared = &metadata->types[generic->declared_type - 1u];
            if (declared->kind != CM_HIR_DECL_TYPE_PRIMITIVE
                || declared->primitive != CM_HIR_DECL_PRIMITIVE_USIZE) {
                cm_free(seen);
                return 0;
            }
        } else if (generic->declared_type != 0u) {
            cm_free(seen);
            return 0;
        }
        if (index != 0u) {
            const CmHirDeclarationGeneric *prior;
            prior = &metadata->generics[index - 1u];
            if (prior->owner_kind > generic->owner_kind
                || (prior->owner_kind == generic->owner_kind
                    && (prior->owner_local > generic->owner_local
                        || (prior->owner_local == generic->owner_local
                            && prior->index >= generic->index)))) {
                cm_free(seen);
                return 0;
            }
        }
    }
    for (owner_index = 0u; owner_index < metadata->item_count;
            ++owner_index) {
        const CmHirDeclarationItem *item;
        uint32_t child;
        item = &metadata->items[owner_index];
        if (!cm_decl_range(item->generic_start, item->generic_count,
                metadata->generic_count)) {
            cm_free(seen);
            return 0;
        }
        for (child = 0u; child < item->generic_count; ++child) {
            size_t local;
            const CmHirDeclarationGeneric *generic;
            local = (size_t)item->generic_start + child - 1u;
            generic = &metadata->generics[local];
            if (seen[local] || generic->owner_kind
                    != CM_HIR_DECL_GENERIC_ITEM
                || generic->owner_local != (uint32_t)(owner_index + 1u)
                || generic->index != child) {
                cm_free(seen);
                return 0;
            }
            seen[local] = 1u;
        }
    }
    for (owner_index = 0u; owner_index < metadata->trait_count;
            ++owner_index) {
        const CmHirDeclarationTrait *trait_value;
        uint32_t child;
        trait_value = &metadata->traits[owner_index];
        if (!cm_decl_range(trait_value->generic_start,
                trait_value->generic_count, metadata->generic_count)) {
            cm_free(seen);
            return 0;
        }
        for (child = 0u; child < trait_value->generic_count; ++child) {
            size_t local;
            const CmHirDeclarationGeneric *generic;
            local = (size_t)trait_value->generic_start + child - 1u;
            generic = &metadata->generics[local];
            if (seen[local] || generic->owner_kind
                    != CM_HIR_DECL_GENERIC_NOMINAL
                || generic->owner_local != (uint32_t)(owner_index + 1u)
                || generic->index != child) {
                cm_free(seen);
                return 0;
            }
            seen[local] = 1u;
        }
    }
    for (owner_index = 0u; owner_index < metadata->value_count;
            ++owner_index) {
        const CmHirDeclarationValue *value;
        uint32_t child;
        value = &metadata->values[owner_index];
        if (!cm_decl_range(value->generic_start, value->generic_count,
                metadata->generic_count)) {
            cm_free(seen);
            return 0;
        }
        for (child = 0u; child < value->generic_count; ++child) {
            size_t local;
            const CmHirDeclarationGeneric *generic;
            local = (size_t)value->generic_start + child - 1u;
            generic = &metadata->generics[local];
            if (seen[local] || generic->owner_kind
                    != CM_HIR_DECL_GENERIC_VALUE
                || generic->owner_local != (uint32_t)(owner_index + 1u)
                || generic->index != child) {
                cm_free(seen);
                return 0;
            }
            seen[local] = 1u;
        }
    }
    for (index = 0u; index < metadata->generic_count; ++index) {
        if (!seen[index]) {
            cm_free(seen);
            return 0;
        }
    }
    cm_free(seen);
    return 1;
}

static int cm_decl_validate_traits(const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t total_supertrait_arguments;
    if (metadata->trait_count != 0u && metadata->traits == NULL) return 0;
    total_supertrait_arguments = 0u;
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirDeclarationTrait *trait_value;
        uint32_t child;
        trait_value = &metadata->traits[index];
        if (trait_value->owner_module == 0u
            || (size_t)trait_value->owner_module > metadata->module_count
            || !cm_decl_identifier(trait_value->name)
            || !cm_decl_visibility_valid(metadata,
                &trait_value->visibility, trait_value->owner_module)
            || !cm_decl_range(trait_value->associated_start,
                trait_value->associated_count, metadata->associated_count)
            || trait_value->predicate_scope_start != 0u
            || trait_value->predicate_scope_count != 0u
            || !cm_decl_range(trait_value->predicate_start,
                trait_value->predicate_count, metadata->predicate_count)
            || !cm_decl_range(trait_value->outlives_start,
                trait_value->outlives_count,
                metadata->outlives_predicate_count)
            || (trait_value->flags
                & (uint8_t)~(CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM
                    | CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
                    | CM_HIR_DECL_TRAIT_IS_CONST
                    | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
                    | CM_HIR_DECL_TRAIT_FUNDAMENTAL
                    | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
                    | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT)) != 0u
            || (trait_value->compiler_flags
                & (uint16_t)~(CM_HIR_DECL_TRAIT_COMPILER_SPECIALIZATION
                    | CM_HIR_DECL_TRAIT_COMPILER_COINDUCTIVE
                    | CM_HIR_DECL_TRAIT_COMPILER_TRIVIAL_FIELD_READS))
                != 0u
            || ((trait_value->flags
                    & CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM) != 0u
                ? !cm_decl_identifier(trait_value->diagnostic_item)
                : (trait_value->diagnostic_item.data != NULL
                    || trait_value->diagnostic_item.length != 0u))
            || ((trait_value->flags
                    & CM_HIR_DECL_TRAIT_HAS_LANG_ITEM) != 0u
                ? !cm_decl_identifier(trait_value->lang_item)
                : (trait_value->lang_item.data != NULL
                    || trait_value->lang_item.length != 0u))
            || ((trait_value->supertrait_count == 0u)
                != (trait_value->supertraits == NULL))
            || trait_value->supertrait_count
                > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || trait_value->safety > CM_HIR_DECL_SAFETY_UNSAFE) return 0;
        for (child = 0u; child < trait_value->supertrait_count; ++child) {
            const CmHirDeclarationSupertrait *supertrait =
                &trait_value->supertraits[child];
            uint32_t argument;
            uint32_t prior;
            if (supertrait->modifier != CM_HIR_DECL_SUPERTRAIT_REQUIRED
                || supertrait->trait_local == 0u
                || (size_t)supertrait->trait_local > metadata->trait_count
                || supertrait->trait_local == (uint32_t)(index + 1u)
                || supertrait->argument_count
                    != metadata->traits[supertrait->trait_local - 1u]
                        .generic_count
                || ((supertrait->argument_count == 0u)
                    != (supertrait->argument_types == NULL))
                || !cm_size_add(total_supertrait_arguments,
                    supertrait->argument_count,
                    &total_supertrait_arguments)
                || total_supertrait_arguments
                    > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES) return 0;
            for (argument = 0u; argument < supertrait->argument_count;
                    ++argument)
                if (!cm_decl_type_local(metadata,
                        supertrait->argument_types[argument])) return 0;
            for (prior = 0u; prior < child; ++prior)
                if (trait_value->supertraits[prior].trait_local
                        == supertrait->trait_local) return 0;
        }
        if (index != 0u) {
            const CmHirDeclarationTrait *prior;
            int order;
            prior = &metadata->traits[index - 1u];
            order = cm_decl_string_compare(prior->name, trait_value->name);
            if (prior->owner_module > trait_value->owner_module
                || (prior->owner_module == trait_value->owner_module
                    && (order > 0 || (order == 0
                        && prior->source_ordinal
                            >= trait_value->source_ordinal)))) return 0;
        }
    }
    return 1;
}

static int cm_decl_validate_associated_items(
    const CmHirDeclarationMetadata *metadata)
{
    unsigned char *seen;
    size_t index;
    size_t owner_index;
    size_t total_parameters;
    if (metadata->associated_count != 0u
        && metadata->associated_items == NULL) return 0;
    seen = metadata->associated_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->associated_count,
            sizeof(*seen));
    total_parameters = 0u;
    for (index = 0u; index < metadata->associated_count; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        const CmHirDeclarationType *receiver_type;
        const CmHirDeclarationType *self_type;
        uint32_t child;
        size_t prior;
        if ((associated->kind != CM_HIR_DECL_ASSOCIATED_TYPE
                && associated->kind != CM_HIR_DECL_ASSOCIATED_METHOD)
            || associated->parent_kind
                != CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL
            || associated->parent_local == 0u
            || (size_t)associated->parent_local > metadata->trait_count
            || associated->implemented_associated_local != 0u
            || !cm_decl_identifier(associated->name)
            || associated->visibility.kind
                != CM_HIR_DECL_VISIBILITY_PRIVATE
            || associated->visibility.restriction_module != 0u
            || associated->is_specializable != 0u
            || associated->generic_start != 0u
            || associated->generic_count != 0u
            || !cm_decl_range(associated->predicate_start,
                associated->predicate_count, metadata->predicate_count)
            || (associated->flags
                & (uint8_t)~CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM) != 0u
            || ((associated->flags
                    & CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM) != 0u
                ? !cm_decl_identifier(associated->lang_item)
                : (associated->lang_item.data != NULL
                    || associated->lang_item.length != 0u))) {
            cm_free(seen);
            return 0;
        }
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_TYPE) {
            if (associated->flags
                    != CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM
                || associated->receiver != CM_HIR_DECL_RECEIVER_NONE
                || associated->parameter_count != 0u
                || associated->parameter_types != NULL
                || associated->return_type != 0u
                || associated->abi.data != NULL
                || associated->abi.length != 0u
                || associated->safety != CM_HIR_DECL_SAFETY_SAFE
                || associated->is_const != 0u
                || associated->is_async != 0u
                || associated->is_variadic != 0u
                || associated->has_default_body != 0u
                || associated->predicate_start != 0u
                || associated->predicate_count != 0u) {
                cm_free(seen);
                return 0;
            }
        } else if ((associated->receiver != CM_HIR_DECL_RECEIVER_VALUE
                    && associated->receiver
                        != CM_HIR_DECL_RECEIVER_REF_SHARED
                    && associated->receiver
                        != CM_HIR_DECL_RECEIVER_REF_MUTABLE)
                || associated->parameter_count == 0u
                || associated->parameter_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || associated->parameter_types == NULL
                || !cm_size_add(total_parameters,
                    associated->parameter_count, &total_parameters)
                || total_parameters > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_decl_type_local(metadata, associated->return_type)
                || (!cm_decl_string_is(associated->abi, "Rust")
                    && !cm_decl_string_is(associated->abi, "rust-call"))
                || associated->safety > CM_HIR_DECL_SAFETY_UNSAFE
                || associated->is_const != 0u
                || associated->is_async != 0u
                || associated->is_variadic != 0u
                || associated->has_default_body > 1u) {
            cm_free(seen);
            return 0;
        }
        for (child = 0u; child < associated->parameter_count; ++child) {
            if (!cm_decl_type_local(metadata,
                    associated->parameter_types[child])) {
                cm_free(seen);
                return 0;
            }
        }
        receiver_type = associated->kind == CM_HIR_DECL_ASSOCIATED_METHOD
            ? &metadata->types[associated->parameter_types[0] - 1u] : NULL;
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_TYPE) {
            self_type = NULL;
        } else if (associated->receiver == CM_HIR_DECL_RECEIVER_VALUE) {
            self_type = receiver_type;
        } else {
            if (receiver_type->kind != CM_HIR_DECL_TYPE_REFERENCE
                || receiver_type->region.kind != CM_HIR_DECL_REGION_ERASED
                || receiver_type->mutability
                    != (associated->receiver
                            == CM_HIR_DECL_RECEIVER_REF_MUTABLE
                        ? CM_HIR_DECL_MUTABLE : CM_HIR_DECL_IMMUTABLE)
                || !cm_decl_type_local(metadata,
                    receiver_type->child_type)) {
                cm_free(seen);
                return 0;
            }
            self_type = &metadata->types[receiver_type->child_type - 1u];
        }
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_METHOD
            && (self_type == NULL
                || self_type->kind != CM_HIR_DECL_TYPE_SELF
                || self_type->self_trait_local
                    != associated->parent_local)) {
            cm_free(seen);
            return 0;
        }
        if (index != 0u) {
            const CmHirDeclarationAssociatedItem *prior_item =
                &metadata->associated_items[index - 1u];
            if (prior_item->parent_kind > associated->parent_kind
                || (prior_item->parent_kind == associated->parent_kind
                    && (prior_item->parent_local > associated->parent_local
                        || (prior_item->parent_local
                                == associated->parent_local
                            && prior_item->source_ordinal
                                >= associated->source_ordinal)))) {
                cm_free(seen);
                return 0;
            }
        }
        for (prior = 0u; prior < index; ++prior) {
            if (metadata->associated_items[prior].parent_kind
                    == associated->parent_kind
                && metadata->associated_items[prior].parent_local
                    == associated->parent_local
                && cm_decl_string_equal(
                    metadata->associated_items[prior].name,
                    associated->name)) {
                cm_free(seen);
                return 0;
            }
        }
    }
    for (owner_index = 0u; owner_index < metadata->trait_count;
            ++owner_index) {
        const CmHirDeclarationTrait *trait_value =
            &metadata->traits[owner_index];
        uint32_t child;
        for (child = 0u; child < trait_value->associated_count; ++child) {
            size_t local = (size_t)trait_value->associated_start + child - 1u;
            const CmHirDeclarationAssociatedItem *associated =
                &metadata->associated_items[local];
            if (seen[local]
                || associated->parent_kind
                    != CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL
                || associated->parent_local != (uint32_t)(owner_index + 1u)) {
                cm_free(seen);
                return 0;
            }
            seen[local] = 1u;
        }
    }
    for (index = 0u; index < metadata->associated_count; ++index) {
        if (!seen[index]) {
            cm_free(seen);
            return 0;
        }
    }
    cm_free(seen);
    return 1;
}

static int cm_decl_variant_name_compare(const void *left_value,
    const void *right_value)
{
    const CmHirDeclarationString *left =
        (const CmHirDeclarationString *)left_value;
    const CmHirDeclarationString *right =
        (const CmHirDeclarationString *)right_value;
    return cm_decl_string_compare(*left, *right);
}

typedef struct CmDeclDiscriminantKey {
    uint64_t low;
    uint64_t high;
} CmDeclDiscriminantKey;

static int cm_decl_discriminant_compare(const void *left_value,
    const void *right_value)
{
    const CmDeclDiscriminantKey *left =
        (const CmDeclDiscriminantKey *)left_value;
    const CmDeclDiscriminantKey *right =
        (const CmDeclDiscriminantKey *)right_value;
    if (left->high != right->high) return left->high < right->high ? -1 : 1;
    if (left->low != right->low) return left->low < right->low ? -1 : 1;
    return 0;
}

static int cm_decl_explicit_enum_repr(uint8_t representation)
{
    return representation == CM_HIR_DECL_ENUM_REPR_U8
        || representation == CM_HIR_DECL_ENUM_REPR_U16
        || representation == CM_HIR_DECL_ENUM_REPR_U32
        || representation == CM_HIR_DECL_ENUM_REPR_U64;
}

static int cm_decl_discriminant_fits(uint8_t representation,
    uint64_t low, uint64_t high)
{
    if (high != UINT64_C(0)) return 0;
    if (representation == CM_HIR_DECL_ENUM_REPR_U8)
        return low <= UINT64_C(255);
    if (representation == CM_HIR_DECL_ENUM_REPR_U16)
        return low <= UINT64_C(65535);
    if (representation == CM_HIR_DECL_ENUM_REPR_U32)
        return low <= UINT64_C(4294967295);
    return representation == CM_HIR_DECL_ENUM_REPR_U64;
}

static int cm_decl_validate_items(const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t total_fields;
    size_t total_variants;
    if (metadata->item_count != 0u && metadata->items == NULL) return 0;
    total_fields = 0u;
    total_variants = 0u;
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item;
        item = &metadata->items[index];
        if ((item->kind != CM_HIR_DECL_ITEM_STRUCT
                && item->kind != CM_HIR_DECL_ITEM_UNION
                && item->kind != CM_HIR_DECL_ITEM_ENUM
                && item->kind != CM_HIR_DECL_ITEM_TYPE_ALIAS)
            || item->owner_module == 0u
            || (size_t)item->owner_module > metadata->module_count
            || !cm_decl_identifier(item->name)
            || !cm_decl_visibility_valid(metadata, &item->visibility,
                item->owner_module)
            || !cm_decl_range(item->generic_start, item->generic_count,
                metadata->generic_count)
            || (item->generic_count != 0u && metadata->generics == NULL)
            || item->predicate_scope_start != 0u
            || item->predicate_scope_count != 0u
            || !cm_decl_range(item->predicate_start,
                item->predicate_count, metadata->predicate_count)
            || (item->predicate_count != 0u
                && item->kind != CM_HIR_DECL_ITEM_STRUCT)
            || item->outlives_start != 0u
            || item->outlives_count != 0u
            || (item->kind != CM_HIR_DECL_ITEM_TYPE_ALIAS
                && item->alias_target_type != 0u)
            || (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
                && !cm_decl_type_local(metadata,
                    item->alias_target_type))
            || (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
                && (item->generic_start != 0u
                    || item->generic_count != 0u))
            || (item->kind != CM_HIR_DECL_ITEM_ENUM
                && (item->enum_repr_primitive != 0u
                    || item->variant_count != 0u
                    || item->variants != NULL
                    || item->enum_flags != 0u
                    || item->enum_lang_item.data != NULL
                    || item->enum_lang_item.length != 0u))) return 0;
        {
            uint32_t generic_index;
            for (generic_index = 0u; generic_index < item->generic_count;
                    ++generic_index) {
                const CmHirDeclarationGeneric *generic =
                    &metadata->generics[(size_t)item->generic_start
                        + generic_index - 1u];
                if (generic->kind == CM_HIR_DECL_GENERIC_CONST
                    && item->kind != CM_HIR_DECL_ITEM_STRUCT) return 0;
            }
        }
        if (item->kind == CM_HIR_DECL_ITEM_STRUCT
                || item->kind == CM_HIR_DECL_ITEM_UNION) {
            CmHirDeclarationString *names;
            uint32_t child;
            uint16_t known_flags =
                CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM
                | CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT
                | CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM
                | CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR;
            if ((item->aggregate_form != CM_HIR_DECL_AGGREGATE_UNIT
                    && item->aggregate_form
                        != CM_HIR_DECL_AGGREGATE_TUPLE
                    && item->aggregate_form
                        != CM_HIR_DECL_AGGREGATE_NAMED)
                || (item->aggregate_repr
                        != CM_HIR_DECL_AGGREGATE_REPR_RUST
                    && item->aggregate_repr
                        != CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT)
                || (item->aggregate_flags & (uint16_t)~known_flags) != 0u
                || ((item->aggregate_flags
                        & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u
                    ? !cm_decl_identifier(item->lang_item)
                    : (item->lang_item.data != NULL
                        || item->lang_item.length != 0u))
                || ((item->aggregate_flags
                        & CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM) != 0u
                    ? !cm_decl_identifier(item->diagnostic_item)
                    : (item->diagnostic_item.data != NULL
                        || item->diagnostic_item.length != 0u))
                || ((item->aggregate_flags
                        & CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT) != 0u
                    && item->aggregate_repr
                        != CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT)
                || ((item->aggregate_flags
                        & CM_HIR_DECL_AGGREGATE_RUSTC_INSIGNIFICANT_DTOR)
                        != 0u
                    && (item->kind != CM_HIR_DECL_ITEM_STRUCT
                        || item->aggregate_form
                            != CM_HIR_DECL_AGGREGATE_NAMED
                        || item->aggregate_repr
                            != CM_HIR_DECL_AGGREGATE_REPR_RUST
                        || (item->aggregate_flags
                                & CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM)
                            == 0u))
                || (item->kind == CM_HIR_DECL_ITEM_UNION
                    && (item->aggregate_form
                            != CM_HIR_DECL_AGGREGATE_NAMED
                        || item->aggregate_repr
                            != CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT))
                || (item->aggregate_form == CM_HIR_DECL_AGGREGATE_UNIT
                    && (item->kind != CM_HIR_DECL_ITEM_STRUCT
                        || item->aggregate_repr
                            != CM_HIR_DECL_AGGREGATE_REPR_RUST
                        || item->aggregate_flags != 0u
                        || item->field_count != 0u
                        || item->fields != NULL))
                || (item->aggregate_form == CM_HIR_DECL_AGGREGATE_TUPLE
                    && (item->kind != CM_HIR_DECL_ITEM_STRUCT
                        || item->field_count == 0u
                        || item->field_count
                            > (uint32_t)CM_HIR_DECL_METADATA_MAX_FIELDS
                        || item->fields == NULL
                        || !cm_size_add(total_fields, item->field_count,
                            &total_fields)
                        || total_fields > CM_HIR_DECL_METADATA_MAX_FIELDS))
                || (item->aggregate_form == CM_HIR_DECL_AGGREGATE_NAMED
                    && (item->field_count == 0u
                        || item->field_count
                            > (uint32_t)CM_HIR_DECL_METADATA_MAX_FIELDS
                        || item->fields == NULL
                        || !cm_size_add(total_fields, item->field_count,
                            &total_fields)
                        || total_fields > CM_HIR_DECL_METADATA_MAX_FIELDS)))
                return 0;
            if (item->aggregate_form == CM_HIR_DECL_AGGREGATE_UNIT)
                names = NULL;
            else names = (CmHirDeclarationString *)cm_alloc(
                (size_t)item->field_count * sizeof(*names));
            for (child = 0u; child < item->field_count; ++child) {
                const CmHirDeclarationField *field = &item->fields[child];
                if ((item->aggregate_form == CM_HIR_DECL_AGGREGATE_TUPLE
                        ? (field->name.data != NULL
                            || field->name.length != 0u
                            || field->visibility.kind
                                != CM_HIR_DECL_VISIBILITY_PRIVATE)
                        : !cm_decl_identifier(field->name))
                    || (field->visibility.kind
                            != CM_HIR_DECL_VISIBILITY_PRIVATE
                        && field->visibility.kind
                            != CM_HIR_DECL_VISIBILITY_PUBLIC
                        && (item->aggregate_form
                                != CM_HIR_DECL_AGGREGATE_NAMED
                            || field->visibility.kind
                                != CM_HIR_DECL_VISIBILITY_CRATE))
                    || field->visibility.restriction_module != 0u
                    || !cm_decl_type_local(metadata, field->type_local)
                    || (child != 0u && item->fields[child - 1u]
                        .source_ordinal >= field->source_ordinal)) {
                    cm_free(names);
                    return 0;
                }
                names[child] = field->name;
            }
            if (item->aggregate_form == CM_HIR_DECL_AGGREGATE_NAMED) {
                qsort(names, item->field_count, sizeof(*names),
                    cm_decl_variant_name_compare);
                for (child = 1u; child < item->field_count; ++child) {
                    if (cm_decl_string_equal(names[child - 1u],
                            names[child])) {
                        cm_free(names);
                        return 0;
                    }
                }
            }
            cm_free(names);
        } else if (item->aggregate_form != 0u
                || item->aggregate_repr != 0u
                || item->aggregate_flags != 0u
                || item->field_count != 0u || item->fields != NULL
                || item->lang_item.data != NULL
                || item->lang_item.length != 0u
                || (item->kind != CM_HIR_DECL_ITEM_ENUM
                    && (item->diagnostic_item.data != NULL
                        || item->diagnostic_item.length != 0u))) {
            return 0;
        } else if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            CmHirDeclarationString *names;
            CmDeclDiscriminantKey *discriminants;
            uint32_t child;
            int explicit_discriminants;
            int has_tuple;
            explicit_discriminants = cm_decl_explicit_enum_repr(
                item->enum_repr_primitive);
            if ((!explicit_discriminants
                    && item->enum_repr_primitive
                        != CM_HIR_DECL_ENUM_REPR_RUST)
                || (item->enum_flags
                    & (uint8_t)~CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u
                || ((item->enum_flags
                        & CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u
                    ? !cm_decl_identifier(item->enum_lang_item)
                    : (item->enum_lang_item.data != NULL
                        || item->enum_lang_item.length != 0u))
                || (explicit_discriminants
                    ? (item->diagnostic_item.data != NULL
                        || item->diagnostic_item.length != 0u
                        || item->generic_count != 0u
                        || item->enum_flags != 0u)
                    : !cm_decl_identifier(item->diagnostic_item))
                || item->variant_count == 0u
                || item->variant_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_VARIANTS
                || item->variants == NULL
                || !cm_decl_range(item->generic_start,
                    item->generic_count, metadata->generic_count)
                || (item->generic_count != 0u
                    && metadata->generics == NULL)
                || !cm_size_add(total_variants, item->variant_count,
                    &total_variants)
                || total_variants > CM_HIR_DECL_METADATA_MAX_VARIANTS)
                return 0;
            for (child = 0u; child < item->generic_count; ++child) {
                size_t generic_index = (size_t)item->generic_start
                    + (size_t)child - 1u;
                if (metadata->generics[generic_index].is_relaxed_sized
                        != 0u)
                    return 0;
            }
            names = (CmHirDeclarationString *)cm_alloc(
                (size_t)item->variant_count * sizeof(*names));
            discriminants = explicit_discriminants
                ? (CmDeclDiscriminantKey *)cm_alloc(
                    (size_t)item->variant_count * sizeof(*discriminants))
                : NULL;
            has_tuple = 0;
            for (child = 0u; child < item->variant_count; ++child) {
                const CmHirDeclarationVariant *variant =
                    &item->variants[child];
                uint32_t field_index;
                if ((variant->kind != CM_HIR_DECL_VARIANT_UNIT
                        && variant->kind != CM_HIR_DECL_VARIANT_TUPLE)
                    || !cm_decl_identifier(variant->name)
                    || (variant->flags
                        & (uint16_t)~CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) != 0u
                    || ((variant->flags
                            & CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) != 0u
                        ? !cm_decl_identifier(variant->lang_item)
                        : (variant->lang_item.data != NULL
                            || variant->lang_item.length != 0u))
                    || (child != 0u && item->variants[child - 1u]
                        .source_ordinal >= variant->source_ordinal)
                    || (variant->kind == CM_HIR_DECL_VARIANT_UNIT
                        && (variant->field_count != 0u
                            || variant->fields != NULL))
                    || (variant->kind == CM_HIR_DECL_VARIANT_TUPLE
                        && (explicit_discriminants
                            || item->generic_count == 0u
                            || variant->field_count == 0u
                            || variant->field_count
                                > (uint32_t)
                                    CM_HIR_DECL_METADATA_MAX_FIELDS
                            || variant->fields == NULL
                            || !cm_size_add(total_fields,
                                variant->field_count, &total_fields)
                            || total_fields
                                > CM_HIR_DECL_METADATA_MAX_FIELDS))
                    || (explicit_discriminants
                        ? (variant->discriminant_primitive
                                != CM_HIR_DECL_PRIMITIVE_ISIZE
                            || !cm_decl_discriminant_fits(
                                item->enum_repr_primitive,
                                variant->discriminant_low,
                                variant->discriminant_high))
                        : (variant->discriminant_primitive
                                != CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT
                            || variant->discriminant_low != UINT64_C(0)
                            || variant->discriminant_high != UINT64_C(0)))) {
                    cm_free(discriminants);
                    cm_free(names);
                    return 0;
                }
                if (variant->kind == CM_HIR_DECL_VARIANT_TUPLE) {
                    has_tuple = 1;
                    for (field_index = 0u;
                            field_index < variant->field_count;
                            ++field_index) {
                        const CmHirDeclarationVariantField *field =
                            &variant->fields[field_index];
                        if (!cm_decl_type_local(metadata,
                                field->type_local)
                            || (field_index != 0u
                                && variant->fields[field_index - 1u]
                                    .source_ordinal
                                    >= field->source_ordinal)) {
                            cm_free(discriminants);
                            cm_free(names);
                            return 0;
                        }
                    }
                }
                if (explicit_discriminants) {
                    discriminants[child].low = variant->discriminant_low;
                    discriminants[child].high = variant->discriminant_high;
                }
                names[child] = variant->name;
            }
            if ((!explicit_discriminants && item->generic_count == 0u
                    && (has_tuple || item->enum_flags != 0u))
                || (!explicit_discriminants && item->generic_count != 0u
                    && (!has_tuple
                        || item->generic_start == 0u))) {
                cm_free(discriminants);
                cm_free(names);
                return 0;
            }
            if (!explicit_discriminants && item->generic_count != 0u) {
                for (child = 0u; child < item->variant_count; ++child) {
                    if ((item->variants[child].flags
                            & CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) == 0u) {
                        cm_free(discriminants);
                        cm_free(names);
                        return 0;
                    }
                }
            } else {
                for (child = 0u; child < item->variant_count; ++child) {
                    if (item->variants[child].flags != 0u) {
                        cm_free(discriminants);
                        cm_free(names);
                        return 0;
                    }
                }
            }
            if (explicit_discriminants) {
                qsort(discriminants, item->variant_count,
                    sizeof(*discriminants), cm_decl_discriminant_compare);
                for (child = 1u; child < item->variant_count; ++child) {
                    if (cm_decl_discriminant_compare(
                            &discriminants[child - 1u],
                            &discriminants[child]) == 0) {
                        cm_free(discriminants);
                        cm_free(names);
                        return 0;
                    }
                }
            }
            qsort(names, item->variant_count, sizeof(*names),
                cm_decl_variant_name_compare);
            for (child = 1u; child < item->variant_count; ++child) {
                if (cm_decl_string_equal(names[child - 1u], names[child])) {
                    cm_free(discriminants);
                    cm_free(names);
                    return 0;
                }
            }
            cm_free(discriminants);
            cm_free(names);
        }
        if ((item->aggregate_flags
                & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u) {
            size_t prior_index;
            for (prior_index = 0u; prior_index < index; ++prior_index) {
                const CmHirDeclarationItem *prior_item =
                    &metadata->items[prior_index];
                if ((prior_item->aggregate_flags
                            & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u
                    && cm_decl_string_equal(prior_item->lang_item,
                        item->lang_item)) return 0;
            }
        }
        if (index != 0u) {
            const CmHirDeclarationItem *prior;
            int order;
            prior = &metadata->items[index - 1u];
            order = cm_decl_string_compare(prior->name, item->name);
            if (prior->owner_module > item->owner_module
                || (prior->owner_module == item->owner_module
                    && (order > 0 || (order == 0
                        && (prior->kind > item->kind
                            || (prior->kind == item->kind
                                && prior->source_ordinal
                                    >= item->source_ordinal)))))) return 0;
        }
    }
    return 1;
}

static int cm_decl_validate_lang_items(
    const CmHirDeclarationMetadata *metadata)
{
    CmHirDeclarationString *names;
    size_t count;
    size_t index;
    size_t cursor;
    count = 0u;
    for (index = 0u; index < metadata->trait_count; ++index) {
        if ((metadata->traits[index].flags
                & CM_HIR_DECL_TRAIT_HAS_LANG_ITEM) != 0u)
            count += 1u;
    }
    for (index = 0u; index < metadata->associated_count; ++index) {
        if ((metadata->associated_items[index].flags
                & CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM) != 0u)
            count += 1u;
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        size_t added = 0u;
        uint32_t child;
        if ((item->aggregate_flags
                & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u)
            added += 1u;
        if ((item->enum_flags & CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u)
            added += 1u;
        for (child = 0u; child < item->variant_count; ++child) {
            if ((item->variants[child].flags
                    & CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) != 0u)
                added += 1u;
        }
        if (!cm_size_add(count, added, &count)
            || count > CM_HIR_DECL_METADATA_MAX_ITEMS
                    + CM_HIR_DECL_METADATA_MAX_VARIANTS) return 0;
    }
    if (count == 0u) return 1;
    if (count > (size_t)CM_HIR_DECL_METADATA_MAX_NOMINALS
            + (size_t)CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS
            + (size_t)CM_HIR_DECL_METADATA_MAX_ITEMS
            + (size_t)CM_HIR_DECL_METADATA_MAX_VARIANTS
        || count > SIZE_MAX / sizeof(*names)) return 0;
    names = (CmHirDeclarationString *)cm_alloc(count * sizeof(*names));
    cursor = 0u;
    for (index = 0u; index < metadata->trait_count; ++index) {
        if ((metadata->traits[index].flags
                & CM_HIR_DECL_TRAIT_HAS_LANG_ITEM) != 0u)
            names[cursor++] = metadata->traits[index].lang_item;
    }
    for (index = 0u; index < metadata->associated_count; ++index) {
        if ((metadata->associated_items[index].flags
                & CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM) != 0u)
            names[cursor++] = metadata->associated_items[index].lang_item;
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        uint32_t child;
        if ((item->aggregate_flags
                & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u)
            names[cursor++] = item->lang_item;
        if ((item->enum_flags & CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u)
            names[cursor++] = item->enum_lang_item;
        for (child = 0u; child < item->variant_count; ++child) {
            if ((item->variants[child].flags
                    & CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) != 0u)
                names[cursor++] = item->variants[child].lang_item;
        }
    }
    qsort(names, count, sizeof(*names), cm_decl_variant_name_compare);
    for (index = 1u; index < count; ++index) {
        if (cm_decl_string_equal(names[index - 1u], names[index])) {
            cm_free(names);
            return 0;
        }
    }
    cm_free(names);
    return 1;
}

static int cm_decl_validate_diagnostic_items(
    const CmHirDeclarationMetadata *metadata)
{
    CmHirDeclarationString *names;
    size_t count = 0u;
    size_t cursor = 0u;
    size_t index;
    for (index = 0u; index < metadata->trait_count; ++index) {
        if ((metadata->traits[index].flags
                & CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM) != 0u)
            count += 1u;
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        if (metadata->items[index].diagnostic_item.data != NULL)
            count += 1u;
    }
    if (count == 0u) return 1;
    if (count > CM_HIR_DECL_METADATA_MAX_NOMINALS
            + CM_HIR_DECL_METADATA_MAX_ITEMS
        || count > SIZE_MAX / sizeof(*names)) return 0;
    names = (CmHirDeclarationString *)cm_alloc(count * sizeof(*names));
    for (index = 0u; index < metadata->trait_count; ++index) {
        if ((metadata->traits[index].flags
                & CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM) != 0u)
            names[cursor++] = metadata->traits[index].diagnostic_item;
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        if (metadata->items[index].diagnostic_item.data != NULL)
            names[cursor++] = metadata->items[index].diagnostic_item;
    }
    qsort(names, count, sizeof(*names), cm_decl_variant_name_compare);
    for (index = 1u; index < count; ++index) {
        if (cm_decl_string_equal(names[index - 1u], names[index])) {
            cm_free(names);
            return 0;
        }
    }
    cm_free(names);
    return 1;
}

static int cm_decl_type_compare(const CmHirDeclarationType *left,
    uint32_t left_depth, const CmHirDeclarationType *right,
    uint32_t right_depth)
{
    uint32_t index;
    if (left_depth != right_depth) return left_depth < right_depth ? -1 : 1;
    if (left->kind != right->kind) return left->kind < right->kind ? -1 : 1;
    if (left->kind == CM_HIR_DECL_TYPE_PRIMITIVE)
        return left->primitive < right->primitive ? -1
            : left->primitive > right->primitive;
    if (left->kind == CM_HIR_DECL_TYPE_GENERIC)
        return left->generic_local < right->generic_local ? -1
            : left->generic_local > right->generic_local;
    if (left->kind == CM_HIR_DECL_TYPE_NAMED_ADT)
        return left->item_local < right->item_local ? -1
            : left->item_local > right->item_local;
    if (left->kind == CM_HIR_DECL_TYPE_SELF)
        return left->self_trait_local < right->self_trait_local ? -1
            : left->self_trait_local > right->self_trait_local;
    if (left->kind == CM_HIR_DECL_TYPE_SLICE)
        return left->child_type < right->child_type ? -1
            : left->child_type > right->child_type;
    if (left->kind == CM_HIR_DECL_TYPE_RAW_POINTER) {
        if (left->mutability != right->mutability)
            return left->mutability < right->mutability ? -1 : 1;
        return left->child_type < right->child_type ? -1
            : left->child_type > right->child_type;
    }
    if (left->kind == CM_HIR_DECL_TYPE_REFERENCE) {
        if (left->region.kind != right->region.kind)
            return left->region.kind < right->region.kind ? -1 : 1;
        if (left->region.generic_local != right->region.generic_local)
            return left->region.generic_local < right->region.generic_local
                ? -1 : 1;
        if (left->region.binder_index != right->region.binder_index)
            return left->region.binder_index < right->region.binder_index
                ? -1 : 1;
        if (left->mutability != right->mutability)
            return left->mutability < right->mutability ? -1 : 1;
        return left->child_type < right->child_type ? -1
            : left->child_type > right->child_type;
    }
    if (left->kind == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
        if (left->item_local != right->item_local)
            return left->item_local < right->item_local ? -1 : 1;
        if (left->argument_count != right->argument_count)
            return left->argument_count < right->argument_count ? -1 : 1;
        for (index = 0u; index < left->argument_count; ++index) {
            if (left->argument_types[index] != right->argument_types[index])
                return left->argument_types[index]
                        < right->argument_types[index] ? -1 : 1;
        }
        return 0;
    }
    if (left->kind == CM_HIR_DECL_TYPE_TUPLE) {
        if (left->element_count != right->element_count)
            return left->element_count < right->element_count ? -1 : 1;
        for (index = 0u; index < left->element_count; ++index) {
            if (left->element_types[index] != right->element_types[index])
                return left->element_types[index]
                        < right->element_types[index] ? -1 : 1;
        }
        return 0;
    }
    if (left->kind == CM_HIR_DECL_TYPE_ARRAY) {
        if (left->child_type != right->child_type)
            return left->child_type < right->child_type ? -1 : 1;
        if (left->array_length_kind != right->array_length_kind)
            return left->array_length_kind < right->array_length_kind
                ? -1 : 1;
        if (left->array_length_kind
                == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER)
            return left->array_length_generic_local
                    < right->array_length_generic_local ? -1
                : left->array_length_generic_local
                    > right->array_length_generic_local;
        if (left->array_length_type != right->array_length_type)
            return left->array_length_type < right->array_length_type
                ? -1 : 1;
        if (left->array_length_low_bits != right->array_length_low_bits)
            return left->array_length_low_bits < right->array_length_low_bits
                ? -1 : 1;
        return left->array_length_high_bits
                < right->array_length_high_bits ? -1
            : left->array_length_high_bits > right->array_length_high_bits;
    }
    if (left->kind == CM_HIR_DECL_TYPE_PROJECTION) {
        if (left->projection_self_type != right->projection_self_type)
            return left->projection_self_type < right->projection_self_type
                ? -1 : 1;
        if (left->projection_trait_local
                != right->projection_trait_local)
            return left->projection_trait_local
                    < right->projection_trait_local ? -1 : 1;
        if (left->projection_associated_local
                != right->projection_associated_local)
            return left->projection_associated_local
                    < right->projection_associated_local ? -1 : 1;
        if (left->projection_argument_count
                != right->projection_argument_count)
            return left->projection_argument_count
                    < right->projection_argument_count ? -1 : 1;
        for (index = 0u; index < left->projection_argument_count; ++index) {
            if (left->projection_argument_types[index]
                    != right->projection_argument_types[index])
                return left->projection_argument_types[index]
                        < right->projection_argument_types[index] ? -1 : 1;
        }
        return 0;
    }
    return 0;
}

static int cm_decl_type_common_zero(const CmHirDeclarationType *type)
{
    return type->primitive == 0u && type->generic_local == 0u
        && type->item_local == 0u && type->child_type == 0u
        && type->self_trait_local == 0u && type->mutability == 0u
        && type->region.kind == 0u && type->region.generic_local == 0u
        && type->region.binder_index == 0u
        && type->argument_count == 0u && type->argument_types == NULL
        && type->element_count == 0u && type->element_types == NULL
        && type->array_length_kind == 0u
        && type->array_length_type == 0u
        && type->array_length_low_bits == UINT64_C(0)
        && type->array_length_high_bits == UINT64_C(0)
        && type->array_length_generic_local == 0u
        && type->projection_self_type == 0u
        && type->projection_trait_local == 0u
        && type->projection_associated_local == 0u
        && type->projection_argument_count == 0u
        && type->projection_argument_types == NULL;
}

static int cm_decl_validate_types(const CmHirDeclarationMetadata *metadata)
{
    uint32_t *depths;
    size_t index;
    size_t total_arguments;
    int valid;
    if (metadata->type_count != 0u && metadata->types == NULL) return 0;
    depths = metadata->type_count == 0u ? NULL
        : (uint32_t *)cm_alloc_zeroed(metadata->type_count,
            sizeof(uint32_t));
    total_arguments = 0u;
    for (index = 0u; index < metadata->generic_count; ++index) {
        if (metadata->generics[index].kind == CM_HIR_DECL_GENERIC_CONST
            && (!cm_size_add(total_arguments, 1u, &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES)) {
            cm_free(depths);
            return 0;
        }
    }
    valid = 1;
    for (index = 0u; index < metadata->type_count && valid; ++index) {
        const CmHirDeclarationType *type;
        uint32_t depth;
        type = &metadata->types[index];
        depth = 0u;
        if (type->kind == CM_HIR_DECL_TYPE_PRIMITIVE) {
            CmHirDeclarationType copy = *type;
            copy.primitive = 0u;
            if (!cm_decl_primitive(type->primitive)
                || !cm_decl_type_common_zero(&copy)) valid = 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_GENERIC) {
            CmHirDeclarationType copy = *type;
            copy.generic_local = 0u;
            if (!cm_decl_generic_local(metadata, type->generic_local)
                || metadata->generics[type->generic_local - 1u].kind
                    != CM_HIR_DECL_GENERIC_TYPE
                || !cm_decl_type_common_zero(&copy)) valid = 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_NAMED_ADT) {
            CmHirDeclarationType copy = *type;
            copy.item_local = 0u;
            if (type->item_local == 0u
                || (size_t)type->item_local > metadata->item_count
                || (metadata->items[type->item_local - 1u].kind
                        != CM_HIR_DECL_ITEM_STRUCT
                    && metadata->items[type->item_local - 1u].kind
                        != CM_HIR_DECL_ITEM_UNION
                    && metadata->items[type->item_local - 1u].kind
                        != CM_HIR_DECL_ITEM_ENUM)
                || metadata->items[type->item_local - 1u].generic_count != 0u
                || !cm_decl_type_common_zero(&copy)) valid = 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_SELF) {
            CmHirDeclarationType copy = *type;
            copy.self_trait_local = 0u;
            if (type->self_trait_local == 0u
                || (size_t)type->self_trait_local > metadata->trait_count
                || !cm_decl_type_common_zero(&copy)) valid = 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_SLICE
                || type->kind == CM_HIR_DECL_TYPE_RAW_POINTER
                || type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            CmHirDeclarationType copy = *type;
            if (type->child_type == 0u
                || (size_t)type->child_type > index) valid = 0;
            else depth = depths[type->child_type - 1u] + UINT32_C(1);
            copy.child_type = 0u;
            if (type->kind == CM_HIR_DECL_TYPE_SLICE) {
                if (!cm_decl_type_common_zero(&copy)) valid = 0;
            } else {
                copy.mutability = 0u;
                if (type->mutability != CM_HIR_DECL_IMMUTABLE
                    && type->mutability != CM_HIR_DECL_MUTABLE) valid = 0;
                if (type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
                    copy.region.kind = 0u;
                    if ((type->region.kind != CM_HIR_DECL_REGION_STATIC
                            && type->region.kind
                                != CM_HIR_DECL_REGION_ERASED)
                        || type->region.generic_local != 0u
                        || type->region.binder_index != 0u) valid = 0;
                    copy.region.generic_local = 0u;
                    copy.region.binder_index = 0u;
                }
                if (!cm_decl_type_common_zero(&copy)) valid = 0;
            }
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            const CmHirDeclarationItem *item;
            CmHirDeclarationType copy = *type;
            uint32_t child;
            uint32_t maximum_depth;
            copy.item_local = 0u;
            copy.argument_count = 0u;
            copy.argument_types = NULL;
            if (type->item_local == 0u
                || (size_t)type->item_local > metadata->item_count
                || type->argument_count == 0u
                || type->argument_types == NULL
                || type->argument_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_arguments, type->argument_count,
                    &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_decl_type_common_zero(&copy)) {
                valid = 0;
                continue;
            }
            item = &metadata->items[type->item_local - 1u];
            if ((item->kind != CM_HIR_DECL_ITEM_STRUCT
                    && item->kind != CM_HIR_DECL_ITEM_UNION
                    && item->kind != CM_HIR_DECL_ITEM_ENUM)
                || item->generic_count != type->argument_count) {
                valid = 0;
                continue;
            }
            maximum_depth = 0u;
            for (child = 0u; child < type->argument_count; ++child) {
                uint32_t argument = type->argument_types[child];
                size_t generic_index = (size_t)item->generic_start
                    + (size_t)child - 1u;
                if (argument == 0u || (size_t)argument > index
                    || metadata->generics[generic_index].kind
                        != CM_HIR_DECL_GENERIC_TYPE) {
                    valid = 0;
                    break;
                }
                if (depths[argument - 1u] > maximum_depth)
                    maximum_depth = depths[argument - 1u];
            }
            depth = maximum_depth + UINT32_C(1);
        } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
            CmHirDeclarationType copy = *type;
            uint32_t child;
            uint32_t maximum_depth;
            copy.element_count = 0u;
            copy.element_types = NULL;
            if (type->element_count == 0u || type->element_types == NULL
                || type->element_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_arguments, type->element_count,
                    &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_decl_type_common_zero(&copy)) {
                valid = 0;
                continue;
            }
            maximum_depth = 0u;
            for (child = 0u; child < type->element_count; ++child) {
                uint32_t element = type->element_types[child];
                if (element == 0u || (size_t)element > index) {
                    valid = 0;
                    break;
                }
                if (depths[element - 1u] > maximum_depth)
                    maximum_depth = depths[element - 1u];
            }
            depth = maximum_depth + UINT32_C(1);
        } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY) {
            CmHirDeclarationType copy = *type;
            const CmHirDeclarationType *length_type = NULL;
            uint32_t maximum_depth;
            copy.child_type = 0u;
            copy.array_length_kind = 0u;
            copy.array_length_type = 0u;
            copy.array_length_low_bits = UINT64_C(0);
            copy.array_length_high_bits = UINT64_C(0);
            copy.array_length_generic_local = 0u;
            if (type->child_type == 0u || (size_t)type->child_type > index
                || !cm_size_add(total_arguments, 2u, &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_decl_type_common_zero(&copy)) {
                valid = 0;
                continue;
            }
            maximum_depth = depths[type->child_type - 1u];
            if (type->array_length_kind
                    == CM_HIR_DECL_ARRAY_LENGTH_SCALAR) {
                if (type->array_length_type == 0u
                    || (size_t)type->array_length_type > index
                    || type->array_length_generic_local != 0u
                    || type->array_length_high_bits != UINT64_C(0)) {
                    valid = 0;
                    continue;
                }
                length_type = &metadata->types[
                    type->array_length_type - 1u];
                if (length_type->kind != CM_HIR_DECL_TYPE_PRIMITIVE
                    || length_type->primitive
                        != CM_HIR_DECL_PRIMITIVE_USIZE) {
                    valid = 0;
                    continue;
                }
                if (depths[type->array_length_type - 1u] > maximum_depth)
                    maximum_depth = depths[type->array_length_type - 1u];
            } else if (type->array_length_kind
                    == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER) {
                const CmHirDeclarationGeneric *generic;
                if (type->array_length_type != 0u
                    || type->array_length_low_bits != UINT64_C(0)
                    || type->array_length_high_bits != UINT64_C(0)
                    || !cm_decl_generic_local(metadata,
                        type->array_length_generic_local)) {
                    valid = 0;
                    continue;
                }
                generic = &metadata->generics[
                    type->array_length_generic_local - 1u];
                if (generic->kind != CM_HIR_DECL_GENERIC_CONST
                    || !cm_decl_type_local(metadata,
                        generic->declared_type)
                    || (size_t)generic->declared_type > index) {
                    valid = 0;
                    continue;
                }
                if (depths[generic->declared_type - 1u] > maximum_depth)
                    maximum_depth = depths[generic->declared_type - 1u];
            } else {
                valid = 0;
                continue;
            }
            depth = maximum_depth + UINT32_C(1);
        } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
            CmHirDeclarationType copy = *type;
            uint32_t child;
            uint32_t maximum_depth;
            copy.projection_self_type = 0u;
            copy.projection_trait_local = 0u;
            copy.projection_associated_local = 0u;
            copy.projection_argument_count = 0u;
            copy.projection_argument_types = NULL;
            if (type->projection_self_type == 0u
                || (size_t)type->projection_self_type > index
                || type->projection_trait_local == 0u
                || (size_t)type->projection_trait_local
                    > metadata->trait_count
                || type->projection_associated_local == 0u
                || (size_t)type->projection_associated_local
                    > metadata->associated_count
                || type->projection_argument_count == 0u
                || type->projection_argument_types == NULL
                || type->projection_argument_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_arguments,
                    type->projection_argument_count, &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_decl_type_common_zero(&copy)) {
                valid = 0;
                continue;
            }
            maximum_depth = depths[type->projection_self_type - 1u];
            for (child = 0u; child < type->projection_argument_count;
                    ++child) {
                uint32_t argument = type->projection_argument_types[child];
                if (argument == 0u || (size_t)argument > index) {
                    valid = 0;
                    break;
                }
                if (depths[argument - 1u] > maximum_depth)
                    maximum_depth = depths[argument - 1u];
            }
            depth = maximum_depth + UINT32_C(1);
        } else {
            valid = 0;
        }
        depths[index] = depth;
        if (valid && index != 0u
            && cm_decl_type_compare(&metadata->types[index - 1u],
                depths[index - 1u], type, depth) >= 0) valid = 0;
    }
    for (index = 0u; index < metadata->item_count && valid; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        const CmHirDeclarationType *target;
        if (item->kind != CM_HIR_DECL_ITEM_TYPE_ALIAS) continue;
        target = &metadata->types[item->alias_target_type - 1u];
        if (target->kind != CM_HIR_DECL_TYPE_NAMED_ADT
            || target->item_local == (uint32_t)(index + 1u)
            || metadata->items[target->item_local - 1u].kind
                != CM_HIR_DECL_ITEM_STRUCT) valid = 0;
    }
    cm_free(depths);
    return valid;
}

static int cm_decl_string_is(CmHirDeclarationString value,
    const char *literal)
{
    size_t length = strlen(literal);
    return value.length == length
        && (length == 0u || memcmp(value.data, literal, length) == 0);
}

/*
 * Validate the bounded aggregate field type profile and the lexical scope of
 * every generic leaf. Children precede parents, so scope propagation and
 * reachability closure are iterative and allocation-bounded.
 */
static int cm_decl_validate_aggregate_types(
    const CmHirDeclarationMetadata *metadata)
{
    uint8_t *scope_kinds;
    uint32_t *scope_locals;
    unsigned char *reachable;
    unsigned char *generic_used;
    size_t index;
    int valid;
    scope_kinds = metadata->type_count == 0u ? NULL
        : (uint8_t *)cm_alloc_zeroed(metadata->type_count, sizeof(uint8_t));
    scope_locals = metadata->type_count == 0u ? NULL
        : (uint32_t *)cm_alloc_zeroed(metadata->type_count,
            sizeof(uint32_t));
    reachable = metadata->type_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->type_count,
            sizeof(unsigned char));
    generic_used = metadata->generic_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->generic_count,
            sizeof(unsigned char));
    valid = 1;
    for (index = 0u; index < metadata->type_count; ++index) {
        const CmHirDeclarationType *type = &metadata->types[index];
        uint8_t kind = 0u;
        uint32_t local = 0u;
        if (type->kind == CM_HIR_DECL_TYPE_GENERIC) {
            const CmHirDeclarationGeneric *generic = &metadata->generics[
                type->generic_local - 1u];
            kind = generic->owner_kind;
            local = generic->owner_local;
        } else if (type->kind == CM_HIR_DECL_TYPE_SLICE
                || type->kind == CM_HIR_DECL_TYPE_RAW_POINTER
                || type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            kind = scope_kinds[type->child_type - 1u];
            local = scope_locals[type->child_type - 1u];
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            uint32_t child;
            for (child = 0u; child < type->argument_count; ++child) {
                uint32_t argument = type->argument_types[child];
                uint8_t argument_kind = scope_kinds[argument - 1u];
                uint32_t argument_local = scope_locals[argument - 1u];
                if (argument_kind == UINT8_MAX
                    || (kind != 0u && argument_kind != 0u
                        && (kind != argument_kind
                            || local != argument_local))) {
                    kind = UINT8_MAX;
                    local = 0u;
                    break;
                }
                if (kind == 0u && argument_kind != 0u) {
                    kind = argument_kind;
                    local = argument_local;
                }
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
            uint32_t child;
            for (child = 0u; child < type->element_count; ++child) {
                uint32_t element = type->element_types[child];
                uint8_t element_kind = scope_kinds[element - 1u];
                uint32_t element_local = scope_locals[element - 1u];
                if (element_kind == UINT8_MAX
                    || (kind != 0u && element_kind != 0u
                        && (kind != element_kind
                            || local != element_local))) {
                    kind = UINT8_MAX;
                    local = 0u;
                    break;
                }
                if (kind == 0u && element_kind != 0u) {
                    kind = element_kind;
                    local = element_local;
                }
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY) {
            uint8_t child_kind = scope_kinds[type->child_type - 1u];
            uint32_t child_local = scope_locals[type->child_type - 1u];
            uint8_t length_kind = 0u;
            uint32_t length_local = 0u;
            if (type->array_length_kind
                    == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER) {
                const CmHirDeclarationGeneric *generic =
                    &metadata->generics[
                        type->array_length_generic_local - 1u];
                length_kind = generic->owner_kind;
                length_local = generic->owner_local;
            }
            if (child_kind == UINT8_MAX || length_kind == UINT8_MAX
                || (child_kind != 0u && length_kind != 0u
                    && (child_kind != length_kind
                        || child_local != length_local))) {
                kind = UINT8_MAX;
                local = 0u;
            } else if (child_kind != 0u) {
                kind = child_kind;
                local = child_local;
            } else {
                kind = length_kind;
                local = length_local;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
            uint32_t child;
            kind = scope_kinds[type->projection_self_type - 1u];
            local = scope_locals[type->projection_self_type - 1u];
            for (child = 0u; child < type->projection_argument_count;
                    ++child) {
                uint32_t argument =
                    type->projection_argument_types[child];
                uint8_t argument_kind = scope_kinds[argument - 1u];
                uint32_t argument_local = scope_locals[argument - 1u];
                if (argument_kind == UINT8_MAX
                    || (kind != 0u && argument_kind != 0u
                        && (kind != argument_kind
                            || local != argument_local))) {
                    kind = UINT8_MAX;
                    local = 0u;
                    break;
                }
                if (kind == 0u && argument_kind != 0u) {
                    kind = argument_kind;
                    local = argument_local;
                }
            }
        }
        scope_kinds[index] = kind;
        scope_locals[index] = local;
    }
    for (index = 0u; index < metadata->item_count && valid; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        uint32_t child;
        uint32_t non_unit_fields = 0u;
        if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t variant_index;
            for (variant_index = 0u;
                    variant_index < item->variant_count && valid;
                    ++variant_index) {
                const CmHirDeclarationVariant *variant =
                    &item->variants[variant_index];
                for (child = 0u; child < variant->field_count; ++child) {
                    uint32_t type_local = variant->fields[child].type_local;
                    uint8_t kind = scope_kinds[type_local - 1u];
                    uint32_t local = scope_locals[type_local - 1u];
                    if (kind != 0u
                        && (kind != CM_HIR_DECL_GENERIC_ITEM
                            || local != (uint32_t)(index + 1u))) {
                        valid = 0;
                        break;
                    }
                    reachable[type_local - 1u] = 1u;
                }
            }
            continue;
        }
        if (item->kind != CM_HIR_DECL_ITEM_STRUCT
            && item->kind != CM_HIR_DECL_ITEM_UNION) continue;
        for (child = 0u; child < item->field_count; ++child) {
            const CmHirDeclarationField *field = &item->fields[child];
            const CmHirDeclarationType *type =
                &metadata->types[field->type_local - 1u];
            uint8_t kind = scope_kinds[field->type_local - 1u];
            uint32_t local = scope_locals[field->type_local - 1u];
            if (kind != 0u && (kind != CM_HIR_DECL_GENERIC_ITEM
                    || local != (uint32_t)(index + 1u))) {
                valid = 0;
                break;
            }
            reachable[field->type_local - 1u] = 1u;
            if (type->kind != CM_HIR_DECL_TYPE_PRIMITIVE
                || type->primitive != CM_HIR_DECL_PRIMITIVE_UNIT)
                non_unit_fields += 1u;
            if (item->kind == CM_HIR_DECL_ITEM_UNION
                && type->kind == CM_HIR_DECL_TYPE_PRIMITIVE
                && type->primitive == CM_HIR_DECL_PRIMITIVE_STR) {
                valid = 0;
                break;
            }
            if (item->kind == CM_HIR_DECL_ITEM_UNION
                && type->kind != CM_HIR_DECL_TYPE_PRIMITIVE) {
                const CmHirDeclarationItem *target;
                if (type->kind
                        != CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
                    valid = 0;
                    break;
                }
                target = &metadata->items[type->item_local - 1u];
                if (target->kind != CM_HIR_DECL_ITEM_STRUCT
                    || target->aggregate_form
                        != CM_HIR_DECL_AGGREGATE_NAMED
                    || target->aggregate_repr
                        != CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
                    || target->generic_count != 1u
                    || target->field_count != 1u
                    || target->fields[0].visibility.kind
                        != CM_HIR_DECL_VISIBILITY_PRIVATE
                    || metadata->types[target->fields[0].type_local - 1u]
                        .kind != CM_HIR_DECL_TYPE_GENERIC
                    || metadata->types[target->fields[0].type_local - 1u]
                        .generic_local != target->generic_start
                    || metadata->generics[target->generic_start - 1u]
                        .is_relaxed_sized != 1u
                    || (target->aggregate_flags
                            & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) == 0u
                    || (target->aggregate_flags
                            & CM_HIR_DECL_AGGREGATE_RUSTC_PUB_TRANSPARENT)
                        == 0u
                    /* This is the compiler-authenticated Rust lang-item
                     * identity, never the source declaration's spelling. */
                    || !cm_decl_string_is(target->lang_item,
                        "manually_drop")) {
                    valid = 0;
                    break;
                }
            }
        }
        if (!valid) break;
        if (item->aggregate_repr
                == CM_HIR_DECL_AGGREGATE_REPR_TRANSPARENT
            && ((item->kind == CM_HIR_DECL_ITEM_STRUCT
                    && item->field_count != 1u)
                || (item->kind == CM_HIR_DECL_ITEM_UNION
                    && non_unit_fields != 1u))) valid = 0;
    }
    for (index = metadata->type_count; index != 0u && valid; --index) {
        const CmHirDeclarationType *type;
        uint32_t child;
        if (!reachable[index - 1u]) continue;
        type = &metadata->types[index - 1u];
        if (type->kind == CM_HIR_DECL_TYPE_GENERIC) {
            generic_used[type->generic_local - 1u] = 1u;
        } else if (type->kind == CM_HIR_DECL_TYPE_SLICE
                || type->kind == CM_HIR_DECL_TYPE_RAW_POINTER
                || type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            reachable[type->child_type - 1u] = 1u;
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            for (child = 0u; child < type->argument_count; ++child)
                reachable[type->argument_types[child] - 1u] = 1u;
        } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
            for (child = 0u; child < type->element_count; ++child)
                reachable[type->element_types[child] - 1u] = 1u;
        } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY) {
            reachable[type->child_type - 1u] = 1u;
            if (type->array_length_kind
                    == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER) {
                const CmHirDeclarationGeneric *generic =
                    &metadata->generics[
                        type->array_length_generic_local - 1u];
                generic_used[type->array_length_generic_local - 1u] = 1u;
                reachable[generic->declared_type - 1u] = 1u;
            } else {
                reachable[type->array_length_type - 1u] = 1u;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
            reachable[type->projection_self_type - 1u] = 1u;
            for (child = 0u; child < type->projection_argument_count;
                    ++child)
                reachable[type->projection_argument_types[child] - 1u] = 1u;
        }
    }
    for (index = 0u; index < metadata->item_count && valid; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        uint32_t child;
        if (item->kind != CM_HIR_DECL_ITEM_ENUM
            && ((item->kind != CM_HIR_DECL_ITEM_STRUCT
                    && item->kind != CM_HIR_DECL_ITEM_UNION)
                || (item->aggregate_form
                        != CM_HIR_DECL_AGGREGATE_NAMED
                    && item->aggregate_form
                        != CM_HIR_DECL_AGGREGATE_TUPLE)))
            continue;
        for (child = 0u; child < item->generic_count; ++child) {
            size_t generic_index =
                (size_t)item->generic_start + child - 1u;
            if (!generic_used[generic_index]) {
                valid = 0;
                break;
            }
        }
    }
    cm_free(generic_used);
    cm_free(reachable);
    cm_free(scope_locals);
    cm_free(scope_kinds);
    return valid;
}

static int cm_decl_zero_generic_unit_function(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationValue *value)
{
    const CmHirDeclarationType *return_type;
    if (value->generic_start != 0u || value->generic_count != 0u
        || value->predicate_start != 0u || value->predicate_count != 0u
        || value->parameter_count != 0u || value->parameter_types != NULL
        || !cm_decl_type_local(metadata, value->return_type)
        || value->has_body != 1u || value->is_const != 0u) return 0;
    return_type = &metadata->types[value->return_type - 1u];
    return return_type->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && return_type->primitive == CM_HIR_DECL_PRIMITIVE_UNIT;
}

static int cm_decl_validate_values(const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t total_parameters;
    if (metadata->value_count != 0u && metadata->values == NULL) return 0;
    total_parameters = 0u;
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirDeclarationValue *value;
        uint32_t child;
        value = &metadata->values[index];
        if (value->owner_module == 0u
            || (size_t)value->owner_module > metadata->module_count
            || !cm_decl_identifier(value->name)
            || !cm_decl_range(value->predicate_start,
                value->predicate_count, metadata->predicate_count)
            || value->has_body > 1u
            || value->is_const > 1u) return 0;
        if (value->kind == CM_HIR_DECL_VALUE_FUNCTION) {
            if ((value->generic_count == 0u
                    && !cm_decl_zero_generic_unit_function(metadata, value))
                || value->parameter_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_parameters, value->parameter_count,
                    &total_parameters)
                || total_parameters > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || ((value->parameter_count == 0u)
                    != (value->parameter_types == NULL))
                || !cm_decl_type_local(metadata, value->return_type)
                || value->declared_type != 0u
                || value->mutability != 0u) return 0;
            for (child = 0u; child < value->parameter_count; ++child) {
                if (!cm_decl_type_local(metadata,
                        value->parameter_types[child])) return 0;
            }
        } else if (value->kind == CM_HIR_DECL_VALUE_CONST
                || value->kind == CM_HIR_DECL_VALUE_STATIC) {
            if (value->generic_start != 0u || value->generic_count != 0u
                || value->predicate_start != 0u
                || value->predicate_count != 0u
                || value->parameter_count != 0u
                || value->parameter_types != NULL
                || value->return_type != 0u
                || !cm_decl_type_local(metadata, value->declared_type)
                || value->has_body != 1u
                || value->is_const != 0u) return 0;
            if (value->kind == CM_HIR_DECL_VALUE_CONST
                && value->mutability != CM_HIR_DECL_IMMUTABLE) return 0;
            if (value->kind == CM_HIR_DECL_VALUE_STATIC
                && value->mutability != CM_HIR_DECL_IMMUTABLE
                && value->mutability != CM_HIR_DECL_MUTABLE) return 0;
        } else {
            return 0;
        }
        if (index != 0u) {
            const CmHirDeclarationValue *prior;
            int order;
            prior = &metadata->values[index - 1u];
            order = cm_decl_string_compare(prior->name, value->name);
            if (prior->owner_module > value->owner_module
                || (prior->owner_module == value->owner_module
                    && (order > 0 || (order == 0
                        && prior->source_ordinal
                            >= value->source_ordinal)))) return 0;
        }
    }
    return 1;
}

static int cm_decl_generic_belongs_to_value(
    const CmHirDeclarationMetadata *metadata, uint32_t generic_local,
    uint32_t value_local)
{
    const CmHirDeclarationGeneric *generic;
    if (!cm_decl_generic_local(metadata, generic_local)) return 0;
    generic = &metadata->generics[generic_local - 1u];
    return generic->owner_kind == CM_HIR_DECL_GENERIC_VALUE
        && generic->owner_local == value_local;
}

static int cm_decl_associated_available_from_trait(
    const CmHirDeclarationMetadata *metadata, uint32_t direct_trait,
    uint32_t associated_local)
{
    unsigned char *seen;
    uint32_t *queue;
    size_t cursor;
    size_t count;
    uint32_t declaring_trait;
    int available;
    if (direct_trait == 0u || (size_t)direct_trait > metadata->trait_count
        || associated_local == 0u
        || (size_t)associated_local > metadata->associated_count
        || metadata->associated_items[associated_local - 1u].kind
            != CM_HIR_DECL_ASSOCIATED_TYPE) return 0;
    declaring_trait = metadata->associated_items[associated_local - 1u]
        .parent_local;
    seen = (unsigned char *)cm_alloc_zeroed(metadata->trait_count,
        sizeof(*seen));
    queue = (uint32_t *)cm_alloc(metadata->trait_count * sizeof(*queue));
    cursor = 0u;
    count = 1u;
    queue[0] = direct_trait;
    seen[direct_trait - 1u] = 1u;
    available = 0;
    while (cursor < count) {
        uint32_t local = queue[cursor++];
        const CmHirDeclarationTrait *trait_value =
            &metadata->traits[local - 1u];
        uint32_t child;
        if (local == declaring_trait) {
            available = 1;
            break;
        }
        for (child = 0u; child < trait_value->supertrait_count; ++child) {
            uint32_t next = trait_value->supertraits[child].trait_local;
            if (!seen[next - 1u]) {
                seen[next - 1u] = 1u;
                queue[count++] = next;
            }
        }
    }
    cm_free(queue);
    cm_free(seen);
    return available;
}

static int cm_decl_validate_predicates(
    const CmHirDeclarationMetadata *metadata)
{
    unsigned char *seen;
    size_t index;
    size_t total_arguments;
    size_t total_equalities;
    if (metadata->predicate_count != 0u && metadata->predicates == NULL)
        return 0;
    seen = metadata->predicate_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->predicate_count,
            sizeof(*seen));
    total_arguments = 0u;
    total_equalities = 0u;
    for (index = 0u; index < metadata->predicate_count; ++index) {
        const CmHirDeclarationPredicate *predicate;
        const CmHirDeclarationTrait *trait_value;
        const CmHirDeclarationType *subject;
        uint32_t owner_local;
        uint32_t child;
        predicate = &metadata->predicates[index];
        if (predicate->owner_kind == CM_HIR_DECL_PREDICATE_OWNER_VALUE) {
            if (predicate->owner_value == 0u
                || (size_t)predicate->owner_value > metadata->value_count
                || predicate->owner_associated != 0u
                || predicate->owner_item != 0u
                || predicate->owner_nominal != 0u) {
                cm_free(seen);
                return 0;
            }
            owner_local = predicate->owner_value;
        } else if (predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED) {
            if (predicate->owner_value != 0u
                || predicate->owner_item != 0u
                || predicate->owner_nominal != 0u
                || predicate->owner_associated == 0u
                || (size_t)predicate->owner_associated
                    > metadata->associated_count
                || metadata->associated_items[
                    predicate->owner_associated - 1u].kind
                    != CM_HIR_DECL_ASSOCIATED_METHOD) {
                cm_free(seen);
                return 0;
            }
            owner_local = predicate->owner_associated;
        } else if (predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL) {
            if (predicate->owner_value != 0u
                || predicate->owner_associated != 0u
                || predicate->owner_item != 0u
                || predicate->owner_nominal == 0u
                || (size_t)predicate->owner_nominal
                    > metadata->trait_count) {
                cm_free(seen);
                return 0;
            }
            owner_local = predicate->owner_nominal;
        } else if (predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_ITEM) {
            if (predicate->owner_value != 0u
                || predicate->owner_associated != 0u
                || predicate->owner_nominal != 0u
                || predicate->owner_item == 0u
                || (size_t)predicate->owner_item > metadata->item_count
                || (metadata->items[predicate->owner_item - 1u].kind
                        != CM_HIR_DECL_ITEM_STRUCT
                    && metadata->items[predicate->owner_item - 1u].kind
                        != CM_HIR_DECL_ITEM_UNION)) {
                cm_free(seen);
                return 0;
            }
            owner_local = predicate->owner_item;
        } else {
            cm_free(seen);
            return 0;
        }
        if (predicate->trait_local == 0u
            || (size_t)predicate->trait_local > metadata->trait_count
            || !cm_decl_type_local(metadata, predicate->subject_type)
            || predicate->modifier > CM_HIR_DECL_PREDICATE_CONST) {
            cm_free(seen);
            return 0;
        }
        trait_value = &metadata->traits[predicate->trait_local - 1u];
        subject = &metadata->types[predicate->subject_type - 1u];
        if ((predicate->owner_kind == CM_HIR_DECL_PREDICATE_OWNER_VALUE
                && (subject->kind != CM_HIR_DECL_TYPE_GENERIC
                    || !cm_decl_generic_belongs_to_value(metadata,
                        subject->generic_local, predicate->owner_value)))
            || (predicate->owner_kind
                    == CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
                && (subject->kind != CM_HIR_DECL_TYPE_SELF
                    || subject->self_trait_local
                        != metadata->associated_items[owner_local - 1u]
                            .parent_local))
            || (predicate->owner_kind
                    == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
                && (subject->kind != CM_HIR_DECL_TYPE_GENERIC
                    || metadata->generics[subject->generic_local - 1u]
                        .owner_kind != CM_HIR_DECL_GENERIC_NOMINAL
                    || metadata->generics[subject->generic_local - 1u]
                        .owner_local != predicate->owner_nominal))
            || (predicate->owner_kind
                    == CM_HIR_DECL_PREDICATE_OWNER_ITEM
                && (subject->kind != CM_HIR_DECL_TYPE_GENERIC
                    || metadata->generics[subject->generic_local - 1u]
                        .owner_kind != CM_HIR_DECL_GENERIC_ITEM
                    || metadata->generics[subject->generic_local - 1u]
                        .owner_local != predicate->owner_item))
            || predicate->argument_count != trait_value->generic_count
            || predicate->argument_count
                > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || !cm_size_add(total_arguments, predicate->argument_count,
                &total_arguments)
            || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || (predicate->argument_count != 0u
                && predicate->argument_types == NULL)
            || (predicate->equality_count == 0u)
                != (predicate->equalities == NULL)
            || predicate->equality_count > UINT32_C(1)
            || !cm_size_add(total_equalities, predicate->equality_count,
                &total_equalities)
            || total_equalities > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES) {
            cm_free(seen);
            return 0;
        }
        if ((predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
                || predicate->owner_kind
                    == CM_HIR_DECL_PREDICATE_OWNER_ITEM)
            && (predicate->argument_count != 0u
                || predicate->equality_count != 0u)) {
            cm_free(seen);
            return 0;
        }
        for (child = 0u; child < predicate->argument_count; ++child) {
            if (!cm_decl_type_local(metadata,
                    predicate->argument_types[child])) {
                cm_free(seen);
                return 0;
            }
            if (metadata->generics[
                    (size_t)trait_value->generic_start + child - 1u].kind
                    != CM_HIR_DECL_GENERIC_TYPE) {
                cm_free(seen);
                return 0;
            }
        }
        for (child = 0u; child < predicate->equality_count; ++child) {
            const CmHirDeclarationPredicateEquality *equality =
                &predicate->equalities[child];
            if (predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_VALUE
                || equality->associated_local == 0u
                || (size_t)equality->associated_local
                    > metadata->associated_count
                || metadata->associated_items[
                    equality->associated_local - 1u].kind
                    != CM_HIR_DECL_ASSOCIATED_TYPE
                || !cm_decl_type_local(metadata, equality->value_type)
                || !cm_decl_associated_available_from_trait(metadata,
                    predicate->trait_local, equality->associated_local)
                || (child != 0u
                    && predicate->equalities[child - 1u].associated_local
                        >= equality->associated_local)) {
                cm_free(seen);
                return 0;
            }
        }
        if (index != 0u) {
            const CmHirDeclarationPredicate *prior;
            uint32_t prior_owner;
            prior = &metadata->predicates[index - 1u];
            prior_owner = prior->owner_kind
                    == CM_HIR_DECL_PREDICATE_OWNER_VALUE
                ? prior->owner_value
                : prior->owner_kind
                    == CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
                    ? prior->owner_associated
                    : prior->owner_kind
                        == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
                        ? prior->owner_nominal : prior->owner_item;
            if (prior->owner_kind > predicate->owner_kind
                || (prior->owner_kind == predicate->owner_kind
                    && (prior_owner > owner_local
                        || (prior_owner == owner_local
                            && prior->ordinal >= predicate->ordinal)))) {
                cm_free(seen);
                return 0;
            }
        }
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirDeclarationValue *value;
        uint32_t child;
        value = &metadata->values[index];
        for (child = 0u; child < value->predicate_count; ++child) {
            const CmHirDeclarationPredicate *predicate;
            size_t local = (size_t)value->predicate_start + child - 1u;
            predicate = &metadata->predicates[local];
            if (seen[local]
                || predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_VALUE
                || predicate->owner_value != (uint32_t)(index + 1u)
                || predicate->ordinal != child) {
                cm_free(seen);
                return 0;
            }
            seen[local] = 1u;
        }
    }
    for (index = 0u; index < metadata->associated_count; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        uint32_t child;
        for (child = 0u; child < associated->predicate_count; ++child) {
            const CmHirDeclarationPredicate *predicate;
            size_t local = (size_t)associated->predicate_start
                + child - 1u;
            predicate = &metadata->predicates[local];
            if (seen[local]
                || predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
                || predicate->owner_associated != (uint32_t)(index + 1u)
                || predicate->ordinal != child) {
                cm_free(seen);
                return 0;
            }
            seen[local] = 1u;
        }
    }
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirDeclarationTrait *trait_value = &metadata->traits[index];
        uint32_t child;
        for (child = 0u; child < trait_value->predicate_count; ++child) {
            const CmHirDeclarationPredicate *predicate;
            size_t local = (size_t)trait_value->predicate_start
                + child - 1u;
            predicate = &metadata->predicates[local];
            if (seen[local]
                || predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
                || predicate->owner_nominal != (uint32_t)(index + 1u)
                || predicate->ordinal != child) {
                cm_free(seen);
                return 0;
            }
            seen[local] = 1u;
        }
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        uint32_t child;
        for (child = 0u; child < item->predicate_count; ++child) {
            const CmHirDeclarationPredicate *predicate;
            size_t local = (size_t)item->predicate_start + child - 1u;
            predicate = &metadata->predicates[local];
            if (seen[local]
                || predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_ITEM
                || predicate->owner_item != (uint32_t)(index + 1u)
                || predicate->ordinal != child) {
                cm_free(seen);
                return 0;
            }
            seen[local] = 1u;
        }
    }
    for (index = 0u; index < metadata->predicate_count; ++index) {
        if (!seen[index]) {
            cm_free(seen);
            return 0;
        }
    }
    cm_free(seen);
    return 1;
}

static int cm_decl_validate_outlives_predicates(
    const CmHirDeclarationMetadata *metadata)
{
    unsigned char *seen;
    size_t index;
    size_t owner_index;
    if (metadata->outlives_predicate_count != 0u
        && metadata->outlives_predicates == NULL) return 0;
    seen = metadata->outlives_predicate_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(
            metadata->outlives_predicate_count, sizeof(*seen));
    for (index = 0u; index < metadata->outlives_predicate_count; ++index) {
        const CmHirDeclarationOutlivesPredicate *predicate =
            &metadata->outlives_predicates[index];
        const CmHirDeclarationType *subject;
        if (predicate->owner_kind != CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
            || predicate->owner_local == 0u
            || (size_t)predicate->owner_local > metadata->trait_count
            || !cm_decl_type_local(metadata, predicate->subject_type)
            || predicate->bound.kind != CM_HIR_DECL_REGION_STATIC
            || predicate->bound.generic_local != 0u
            || predicate->bound.binder_index != 0u
            || predicate->scope != 0u) {
            cm_free(seen);
            return 0;
        }
        subject = &metadata->types[predicate->subject_type - 1u];
        if (subject->kind != CM_HIR_DECL_TYPE_SELF
            || subject->self_trait_local != predicate->owner_local) {
            cm_free(seen);
            return 0;
        }
        if (index != 0u) {
            const CmHirDeclarationOutlivesPredicate *prior =
                &metadata->outlives_predicates[index - 1u];
            if (prior->owner_local > predicate->owner_local
                || (prior->owner_local == predicate->owner_local
                    && prior->ordinal >= predicate->ordinal)) {
                cm_free(seen);
                return 0;
            }
        }
    }
    for (owner_index = 0u; owner_index < metadata->trait_count;
            ++owner_index) {
        const CmHirDeclarationTrait *trait_value =
            &metadata->traits[owner_index];
        uint32_t child;
        for (child = 0u; child < trait_value->outlives_count; ++child) {
            size_t local = (size_t)trait_value->outlives_start + child - 1u;
            const CmHirDeclarationOutlivesPredicate *predicate =
                &metadata->outlives_predicates[local];
            if (seen[local]
                || predicate->owner_local != (uint32_t)(owner_index + 1u)
                || predicate->ordinal != child) {
                cm_free(seen);
                return 0;
            }
            seen[local] = 1u;
        }
    }
    for (index = 0u; index < metadata->outlives_predicate_count; ++index) {
        if (!seen[index]) {
            cm_free(seen);
            return 0;
        }
    }
    cm_free(seen);
    return 1;
}

static int cm_decl_validate_type_reachability(
    const CmHirDeclarationMetadata *metadata)
{
    unsigned char *seen;
    size_t index;
    seen = metadata->type_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->type_count,
            sizeof(unsigned char));
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirDeclarationValue *value;
        uint32_t child;
        value = &metadata->values[index];
        if (value->kind == CM_HIR_DECL_VALUE_FUNCTION) {
            seen[value->return_type - 1u] = 1u;
            for (child = 0u; child < value->parameter_count; ++child)
                seen[value->parameter_types[child] - 1u] = 1u;
        } else {
            seen[value->declared_type - 1u] = 1u;
        }
    }
    for (index = 0u; index < metadata->associated_count; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        uint32_t child;
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_TYPE) continue;
        seen[associated->return_type - 1u] = 1u;
        for (child = 0u; child < associated->parameter_count; ++child)
            seen[associated->parameter_types[child] - 1u] = 1u;
    }
    for (index = 0u; index < metadata->predicate_count; ++index) {
        const CmHirDeclarationPredicate *predicate;
        uint32_t child;
        predicate = &metadata->predicates[index];
        seen[predicate->subject_type - 1u] = 1u;
        for (child = 0u; child < predicate->argument_count; ++child)
            seen[predicate->argument_types[child] - 1u] = 1u;
        for (child = 0u; child < predicate->equality_count; ++child)
            seen[predicate->equalities[child].value_type - 1u] = 1u;
    }
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirDeclarationTrait *trait_value = &metadata->traits[index];
        uint32_t child;
        for (child = 0u; child < trait_value->supertrait_count; ++child) {
            const CmHirDeclarationSupertrait *supertrait =
                &trait_value->supertraits[child];
            uint32_t argument;
            for (argument = 0u; argument < supertrait->argument_count;
                    ++argument)
                seen[supertrait->argument_types[argument] - 1u] = 1u;
        }
    }
    for (index = 0u; index < metadata->outlives_predicate_count; ++index)
        seen[metadata->outlives_predicates[index].subject_type - 1u] = 1u;
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        uint32_t child;
        if (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS)
            seen[item->alias_target_type - 1u] = 1u;
        for (child = 0u; child < item->field_count; ++child)
            seen[item->fields[child].type_local - 1u] = 1u;
        if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t variant_index;
            for (variant_index = 0u;
                    variant_index < item->variant_count; ++variant_index) {
                const CmHirDeclarationVariant *variant =
                    &item->variants[variant_index];
                for (child = 0u; child < variant->field_count; ++child)
                    seen[variant->fields[child].type_local - 1u] = 1u;
            }
        }
    }
    /* Canonical children precede parents, so one reverse pass closes roots. */
    for (index = metadata->type_count; index != 0u; --index) {
        const CmHirDeclarationType *type;
        uint32_t child;
        if (!seen[index - 1u]) continue;
        type = &metadata->types[index - 1u];
        if (type->kind == CM_HIR_DECL_TYPE_SLICE
            || type->kind == CM_HIR_DECL_TYPE_RAW_POINTER
            || type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            seen[type->child_type - 1u] = 1u;
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            for (child = 0u; child < type->argument_count; ++child)
                seen[type->argument_types[child] - 1u] = 1u;
        } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
            for (child = 0u; child < type->element_count; ++child)
                seen[type->element_types[child] - 1u] = 1u;
        } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY) {
            seen[type->child_type - 1u] = 1u;
            if (type->array_length_kind
                    == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER) {
                const CmHirDeclarationGeneric *generic =
                    &metadata->generics[
                        type->array_length_generic_local - 1u];
                seen[generic->declared_type - 1u] = 1u;
            } else {
                seen[type->array_length_type - 1u] = 1u;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
            seen[type->projection_self_type - 1u] = 1u;
            for (child = 0u; child < type->projection_argument_count;
                    ++child)
                seen[type->projection_argument_types[child] - 1u] = 1u;
        }
    }
    for (index = 0u; index < metadata->type_count; ++index) {
        if (!seen[index]) {
            cm_free(seen);
            return 0;
        }
    }
    cm_free(seen);
    return 1;
}

/*
 * Record the sole VALUE owner of every generic leaf below each canonical
 * type. Zero means closed; UINT32_MAX means an ITEM/NOMINAL generic or a mix
 * of distinct VALUE scopes. Children precede parents, so no recursion or
 * attacker-controlled stack depth is required.
 */
static int cm_decl_validate_value_type_scopes(
    const CmHirDeclarationMetadata *metadata)
{
    uint32_t *scopes;
    size_t index;
    int valid;
    scopes = metadata->type_count == 0u ? NULL
        : (uint32_t *)cm_alloc_zeroed(metadata->type_count,
            sizeof(uint32_t));
    valid = 1;
    for (index = 0u; index < metadata->type_count; ++index) {
        const CmHirDeclarationType *type = &metadata->types[index];
        uint32_t scope = 0u;
        if (type->kind == CM_HIR_DECL_TYPE_GENERIC) {
            const CmHirDeclarationGeneric *generic = &metadata->generics[
                type->generic_local - 1u];
            scope = generic->owner_kind == CM_HIR_DECL_GENERIC_VALUE
                ? generic->owner_local : UINT32_MAX;
        } else if (type->kind == CM_HIR_DECL_TYPE_SLICE
                || type->kind == CM_HIR_DECL_TYPE_RAW_POINTER
                || type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            scope = scopes[type->child_type - 1u];
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            uint32_t child;
            for (child = 0u; child < type->argument_count; ++child) {
                uint32_t argument_scope = scopes[
                    type->argument_types[child] - 1u];
                if (argument_scope == UINT32_MAX
                    || (scope != 0u && argument_scope != 0u
                        && scope != argument_scope)) {
                    scope = UINT32_MAX;
                    break;
                }
                if (scope == 0u) scope = argument_scope;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
            uint32_t child;
            for (child = 0u; child < type->element_count; ++child) {
                uint32_t element_scope =
                    scopes[type->element_types[child] - 1u];
                if (element_scope == UINT32_MAX
                    || (scope != 0u && element_scope != 0u
                        && scope != element_scope)) {
                    scope = UINT32_MAX;
                    break;
                }
                if (scope == 0u) scope = element_scope;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY) {
            uint32_t child_scope = scopes[type->child_type - 1u];
            uint32_t length_scope = 0u;
            if (type->array_length_kind
                    == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER) {
                const CmHirDeclarationGeneric *generic =
                    &metadata->generics[
                        type->array_length_generic_local - 1u];
                length_scope = generic->owner_kind
                        == CM_HIR_DECL_GENERIC_VALUE
                    ? generic->owner_local : UINT32_MAX;
            }
            if (child_scope == UINT32_MAX || length_scope == UINT32_MAX
                || (child_scope != 0u && length_scope != 0u
                    && child_scope != length_scope)) {
                scope = UINT32_MAX;
            } else {
                scope = child_scope != 0u ? child_scope : length_scope;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
            uint32_t child;
            scope = scopes[type->projection_self_type - 1u];
            for (child = 0u; child < type->projection_argument_count;
                    ++child) {
                uint32_t child_scope = scopes[
                    type->projection_argument_types[child] - 1u];
                if (child_scope == UINT32_MAX
                    || (scope != 0u && child_scope != 0u
                        && scope != child_scope)) {
                    scope = UINT32_MAX;
                    break;
                }
                if (scope == 0u) scope = child_scope;
            }
        }
        scopes[index] = scope;
    }
    for (index = 0u; index < metadata->value_count && valid; ++index) {
        const CmHirDeclarationValue *value = &metadata->values[index];
        uint32_t owner = (uint32_t)(index + 1u);
        uint32_t child;
        uint32_t scope;
        if (value->kind == CM_HIR_DECL_VALUE_CONST
                || value->kind == CM_HIR_DECL_VALUE_STATIC) {
            if (scopes[value->declared_type - 1u] != 0u) valid = 0;
        } else {
            scope = scopes[value->return_type - 1u];
            if (scope != 0u && scope != owner) valid = 0;
            for (child = 0u; child < value->parameter_count && valid;
                    ++child) {
                scope = scopes[value->parameter_types[child] - 1u];
                if (scope != 0u && scope != owner) valid = 0;
            }
        }
    }
    for (index = 0u; index < metadata->associated_count && valid; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        uint32_t child;
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_TYPE) continue;
        if (scopes[associated->return_type - 1u] != 0u
            && (!cm_decl_string_is(associated->abi, "rust-call")
                || scopes[associated->return_type - 1u] != UINT32_MAX))
            valid = 0;
        for (child = 0u; child < associated->parameter_count && valid;
                ++child) {
            if (scopes[associated->parameter_types[child] - 1u] != 0u
                && (!cm_decl_string_is(associated->abi, "rust-call")
                    || scopes[associated->parameter_types[child] - 1u]
                        != UINT32_MAX))
                valid = 0;
        }
    }
    for (index = 0u; index < metadata->predicate_count && valid; ++index) {
        const CmHirDeclarationPredicate *predicate =
            &metadata->predicates[index];
        uint32_t owner = predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_VALUE
            ? predicate->owner_value : 0u;
        uint32_t child;
        uint32_t scope;
        if (predicate->owner_kind == CM_HIR_DECL_PREDICATE_OWNER_ITEM
            || predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL)
            continue;
        scope = scopes[predicate->subject_type - 1u];
        if (scope != 0u && scope != owner) valid = 0;
        for (child = 0u; child < predicate->argument_count && valid;
                ++child) {
            scope = scopes[predicate->argument_types[child] - 1u];
            if (scope != 0u && scope != owner) valid = 0;
        }
        for (child = 0u; child < predicate->equality_count && valid;
                ++child) {
            scope = scopes[predicate->equalities[child].value_type - 1u];
            if (scope != 0u && scope != owner) valid = 0;
        }
    }
    for (index = 0u; index < metadata->outlives_predicate_count && valid;
            ++index) {
        const CmHirDeclarationOutlivesPredicate *predicate =
            &metadata->outlives_predicates[index];
        if (scopes[predicate->subject_type - 1u] != 0u) valid = 0;
    }
    cm_free(scopes);
    return valid;
}

/*
 * Record the sole nominal trait named by every SELF leaf. Zero is no SELF;
 * UINT32_MAX is a mix of distinct traits. Associated method roots may name
 * only their exact parent trait, while every free/item root must remain zero.
 */
static int cm_decl_validate_self_type_scopes(
    const CmHirDeclarationMetadata *metadata)
{
    uint32_t *scopes;
    size_t index;
    int valid;
    scopes = metadata->type_count == 0u ? NULL
        : (uint32_t *)cm_alloc_zeroed(metadata->type_count,
            sizeof(*scopes));
    valid = 1;
    for (index = 0u; index < metadata->type_count; ++index) {
        const CmHirDeclarationType *type = &metadata->types[index];
        uint32_t scope = 0u;
        if (type->kind == CM_HIR_DECL_TYPE_SELF) {
            scope = type->self_trait_local;
        } else if (type->kind == CM_HIR_DECL_TYPE_SLICE
                || type->kind == CM_HIR_DECL_TYPE_RAW_POINTER
                || type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            scope = scopes[type->child_type - 1u];
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            uint32_t child;
            for (child = 0u; child < type->argument_count; ++child) {
                uint32_t child_scope = scopes[
                    type->argument_types[child] - 1u];
                if (child_scope == UINT32_MAX
                    || (scope != 0u && child_scope != 0u
                        && scope != child_scope)) {
                    scope = UINT32_MAX;
                    break;
                }
                if (scope == 0u) scope = child_scope;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
            uint32_t child;
            for (child = 0u; child < type->element_count; ++child) {
                uint32_t child_scope = scopes[
                    type->element_types[child] - 1u];
                if (child_scope == UINT32_MAX
                    || (scope != 0u && child_scope != 0u
                        && scope != child_scope)) {
                    scope = UINT32_MAX;
                    break;
                }
                if (scope == 0u) scope = child_scope;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY) {
            uint32_t child_scope = scopes[type->child_type - 1u];
            uint32_t length_scope = type->array_length_kind
                    == CM_HIR_DECL_ARRAY_LENGTH_SCALAR
                ? scopes[type->array_length_type - 1u] : 0u;
            if (child_scope == UINT32_MAX || length_scope == UINT32_MAX
                || (child_scope != 0u && length_scope != 0u
                    && child_scope != length_scope)) {
                scope = UINT32_MAX;
            } else {
                scope = child_scope != 0u ? child_scope : length_scope;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
            uint32_t child;
            scope = scopes[type->projection_self_type - 1u];
            for (child = 0u; child < type->projection_argument_count;
                    ++child) {
                uint32_t child_scope = scopes[
                    type->projection_argument_types[child] - 1u];
                if (child_scope == UINT32_MAX
                    || (scope != 0u && child_scope != 0u
                        && scope != child_scope)) {
                    scope = UINT32_MAX;
                    break;
                }
                if (scope == 0u) scope = child_scope;
            }
        }
        scopes[index] = scope;
    }
    for (index = 0u; index < metadata->value_count && valid; ++index) {
        const CmHirDeclarationValue *value = &metadata->values[index];
        uint32_t child;
        if (value->kind == CM_HIR_DECL_VALUE_FUNCTION) {
            if (scopes[value->return_type - 1u] != 0u) valid = 0;
            for (child = 0u; child < value->parameter_count && valid;
                    ++child) {
                if (scopes[value->parameter_types[child] - 1u] != 0u)
                    valid = 0;
            }
        } else if (scopes[value->declared_type - 1u] != 0u) {
            valid = 0;
        }
    }
    for (index = 0u; index < metadata->item_count && valid; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        uint32_t child;
        if (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
            && scopes[item->alias_target_type - 1u] != 0u) valid = 0;
        for (child = 0u; child < item->field_count && valid; ++child) {
            if (scopes[item->fields[child].type_local - 1u] != 0u)
                valid = 0;
        }
        if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t variant;
            for (variant = 0u; variant < item->variant_count && valid;
                    ++variant) {
                uint32_t field;
                for (field = 0u;
                        field < item->variants[variant].field_count;
                        ++field) {
                    if (scopes[item->variants[variant].fields[field]
                            .type_local - 1u] != 0u) {
                        valid = 0;
                        break;
                    }
                }
            }
        }
    }
    for (index = 0u; index < metadata->associated_count && valid; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        uint32_t child;
        uint32_t expected = associated->parent_local;
        uint32_t scope;
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_TYPE) continue;
        scope = scopes[associated->return_type - 1u];
        if (scope != 0u && scope != expected) valid = 0;
        for (child = 0u; child < associated->parameter_count && valid;
                ++child) {
            scope = scopes[associated->parameter_types[child] - 1u];
            if (scope != 0u && scope != expected) valid = 0;
        }
    }
    for (index = 0u; index < metadata->predicate_count && valid; ++index) {
        const CmHirDeclarationPredicate *predicate =
            &metadata->predicates[index];
        uint32_t expected = 0u;
        uint32_t child;
        uint32_t scope;
        if (predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED) {
            expected = metadata->associated_items[
                predicate->owner_associated - 1u].parent_local;
        } else if (predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL) {
            expected = predicate->owner_nominal;
        }
        scope = scopes[predicate->subject_type - 1u];
        if (scope != 0u && scope != expected) valid = 0;
        for (child = 0u; child < predicate->argument_count && valid;
                ++child) {
            scope = scopes[predicate->argument_types[child] - 1u];
            if (scope != 0u && scope != expected) valid = 0;
        }
        for (child = 0u; child < predicate->equality_count && valid;
                ++child) {
            scope = scopes[predicate->equalities[child].value_type - 1u];
            if (scope != 0u && scope != expected) valid = 0;
        }
    }
    for (index = 0u; index < metadata->trait_count && valid; ++index) {
        const CmHirDeclarationTrait *trait_value = &metadata->traits[index];
        uint32_t child;
        for (child = 0u; child < trait_value->supertrait_count && valid;
                ++child) {
            const CmHirDeclarationSupertrait *supertrait =
                &trait_value->supertraits[child];
            uint32_t argument;
            for (argument = 0u; argument < supertrait->argument_count;
                    ++argument) {
                uint32_t scope = scopes[
                    supertrait->argument_types[argument] - 1u];
                if (scope != 0u && scope != (uint32_t)(index + 1u)) {
                    valid = 0;
                    break;
                }
            }
        }
    }
    for (index = 0u; index < metadata->outlives_predicate_count && valid;
            ++index) {
        const CmHirDeclarationOutlivesPredicate *predicate =
            &metadata->outlives_predicates[index];
        if (scopes[predicate->subject_type - 1u]
                != predicate->owner_local) valid = 0;
    }
    cm_free(scopes);
    return valid;
}

static int cm_decl_bounded_type_name_of_val_erased_input(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationValue *value, uint32_t value_local)
{
    const CmHirDeclarationGeneric *generic;
    const CmHirDeclarationType *parameter;
    const CmHirDeclarationType *parameter_child;
    const CmHirDeclarationType *result;
    const CmHirDeclarationType *result_child;
    if (value->kind != CM_HIR_DECL_VALUE_FUNCTION
        || value->is_const != 1u || value->has_body != 1u
        || value->generic_count != 1u || value->generic_start == 0u
        || value->predicate_start != 0u || value->predicate_count != 0u
        || value->parameter_count != 1u
        || value->parameter_types == NULL) {
        return 0;
    }
    generic = &metadata->generics[value->generic_start - 1u];
    parameter = &metadata->types[value->parameter_types[0] - 1u];
    result = &metadata->types[value->return_type - 1u];
    if (generic->owner_kind != CM_HIR_DECL_GENERIC_VALUE
        || generic->owner_local != value_local || generic->index != 0u
        || generic->kind != CM_HIR_DECL_GENERIC_TYPE
        || generic->is_relaxed_sized != 1u
        || parameter->kind != CM_HIR_DECL_TYPE_REFERENCE
        || parameter->mutability != CM_HIR_DECL_IMMUTABLE
        || parameter->region.kind != CM_HIR_DECL_REGION_ERASED
        || result->kind != CM_HIR_DECL_TYPE_REFERENCE
        || result->mutability != CM_HIR_DECL_IMMUTABLE
        || result->region.kind != CM_HIR_DECL_REGION_STATIC) return 0;
    parameter_child = &metadata->types[parameter->child_type - 1u];
    result_child = &metadata->types[result->child_type - 1u];
    return parameter_child->kind == CM_HIR_DECL_TYPE_GENERIC
        && parameter_child->generic_local == value->generic_start
        && result_child->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && result_child->primitive == CM_HIR_DECL_PRIMITIVE_STR;
}

/*
 * The source-elision rule for a free function with exactly one borrowed
 * input relates every omitted output lifetime to that input.  The producer
 * normalizes the two independently allocated HIR inference regions only
 * after authenticating this exact source shape.  ERASED therefore carries a
 * single, bounded relation here; it is not admitted as a general free-
 * function lifetime.
 */
static int cm_decl_bounded_array_from_ref_erased_pair(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationValue *value, uint32_t value_local)
{
    const CmHirDeclarationGeneric *generic;
    const CmHirDeclarationType *parameter;
    const CmHirDeclarationType *parameter_child;
    const CmHirDeclarationType *result;
    const CmHirDeclarationType *array;
    const CmHirDeclarationType *array_child;
    const CmHirDeclarationType *length_type;
    if (value->kind != CM_HIR_DECL_VALUE_FUNCTION
        || value->is_const != 1u || value->has_body != 1u
        || value->generic_count != 1u || value->generic_start == 0u
        || value->predicate_start != 0u || value->predicate_count != 0u
        || value->parameter_count != 1u
        || value->parameter_types == NULL) {
        return 0;
    }
    generic = &metadata->generics[value->generic_start - 1u];
    parameter = &metadata->types[value->parameter_types[0] - 1u];
    result = &metadata->types[value->return_type - 1u];
    if (generic->owner_kind != CM_HIR_DECL_GENERIC_VALUE
        || generic->owner_local != value_local || generic->index != 0u
        || generic->kind != CM_HIR_DECL_GENERIC_TYPE
        || generic->is_relaxed_sized != 0u
        || generic->has_default != 0u || generic->declared_type != 0u
        || parameter->kind != CM_HIR_DECL_TYPE_REFERENCE
        || (parameter->mutability != CM_HIR_DECL_IMMUTABLE
            && parameter->mutability != CM_HIR_DECL_MUTABLE)
        || parameter->region.kind != CM_HIR_DECL_REGION_ERASED
        || result->kind != CM_HIR_DECL_TYPE_REFERENCE
        || result->mutability != parameter->mutability
        || result->region.kind != CM_HIR_DECL_REGION_ERASED) return 0;
    parameter_child = &metadata->types[parameter->child_type - 1u];
    array = &metadata->types[result->child_type - 1u];
    if (parameter_child->kind != CM_HIR_DECL_TYPE_GENERIC
        || parameter_child->generic_local != value->generic_start
        || array->kind != CM_HIR_DECL_TYPE_ARRAY
        || array->array_length_kind != CM_HIR_DECL_ARRAY_LENGTH_SCALAR
        || array->array_length_low_bits != UINT64_C(1)
        || array->array_length_high_bits != UINT64_C(0)) return 0;
    array_child = &metadata->types[array->child_type - 1u];
    length_type = &metadata->types[array->array_length_type - 1u];
    return array_child->kind == CM_HIR_DECL_TYPE_GENERIC
        && array_child->generic_local == value->generic_start
        && length_type->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && length_type->primitive == CM_HIR_DECL_PRIMITIVE_USIZE;
}

static int cm_decl_bounded_free_erased_value(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationValue *value, uint32_t value_local)
{
    return cm_decl_bounded_type_name_of_val_erased_input(metadata, value,
            value_local)
        || cm_decl_bounded_array_from_ref_erased_pair(metadata, value,
            value_local);
}

static int cm_decl_bounded_clone_from_erased_source(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationAssociatedItem *associated)
{
    const CmHirDeclarationTrait *parent;
    const CmHirDeclarationType *source;
    const CmHirDeclarationType *source_self;
    const CmHirDeclarationType *result;
    if (associated->parent_local == 0u
        || (size_t)associated->parent_local > metadata->trait_count
        || associated->kind != CM_HIR_DECL_ASSOCIATED_METHOD
        || associated->receiver != CM_HIR_DECL_RECEIVER_REF_MUTABLE
        || associated->parameter_count != 2u
        || associated->parameter_types == NULL
        || !cm_decl_string_is(associated->name, "clone_from")
        || !cm_decl_string_is(associated->abi, "Rust")
        || associated->safety != CM_HIR_DECL_SAFETY_SAFE
        || associated->is_const != 0u || associated->is_async != 0u
        || associated->is_variadic != 0u
        || associated->has_default_body != 1u) return 0;
    parent = &metadata->traits[associated->parent_local - 1u];
    source = &metadata->types[associated->parameter_types[1] - 1u];
    result = &metadata->types[associated->return_type - 1u];
    if ((parent->flags & CM_HIR_DECL_TRAIT_HAS_LANG_ITEM) == 0u
        || !cm_decl_string_is(parent->lang_item, "clone")
        || source->kind != CM_HIR_DECL_TYPE_REFERENCE
        || source->mutability != CM_HIR_DECL_IMMUTABLE
        || source->region.kind != CM_HIR_DECL_REGION_ERASED
        || result->kind != CM_HIR_DECL_TYPE_PRIMITIVE
        || result->primitive != CM_HIR_DECL_PRIMITIVE_UNIT) return 0;
    source_self = &metadata->types[source->child_type - 1u];
    return source_self->kind == CM_HIR_DECL_TYPE_SELF
        && source_self->self_trait_local == associated->parent_local;
}

/*
 * ERASED is a deliberately lossy transport marker, not a general lifetime or
 * binder.  Validate every declaration root that can reach one so a canonical
 * TYPE node cannot be shared from an authenticated receiver/input boundary
 * into an unrelated field, predicate, value, or parameter.
 */
static int cm_decl_validate_erased_type_roots(
    const CmHirDeclarationMetadata *metadata)
{
    unsigned char *contains_erased;
    size_t index;
    int valid = 1;
    contains_erased = metadata->type_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->type_count,
            sizeof(*contains_erased));
    for (index = 0u; index < metadata->type_count; ++index) {
        const CmHirDeclarationType *type = &metadata->types[index];
        uint32_t child;
        if (type->kind == CM_HIR_DECL_TYPE_REFERENCE
            && type->region.kind == CM_HIR_DECL_REGION_ERASED) {
            contains_erased[index] = 1u;
        }
        if (type->kind == CM_HIR_DECL_TYPE_SLICE
                || type->kind == CM_HIR_DECL_TYPE_RAW_POINTER
                || type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            if (contains_erased[type->child_type - 1u])
                contains_erased[index] = 1u;
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            for (child = 0u; child < type->argument_count; ++child) {
                if (contains_erased[type->argument_types[child] - 1u])
                    contains_erased[index] = 1u;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
            for (child = 0u; child < type->element_count; ++child) {
                if (contains_erased[type->element_types[child] - 1u])
                    contains_erased[index] = 1u;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY
            && contains_erased[type->child_type - 1u]) {
            contains_erased[index] = 1u;
        } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
            if (contains_erased[type->projection_self_type - 1u])
                contains_erased[index] = 1u;
            for (child = 0u; child < type->projection_argument_count;
                    ++child) {
                if (contains_erased[
                        type->projection_argument_types[child] - 1u])
                    contains_erased[index] = 1u;
            }
        }
    }
    for (index = 0u; index < metadata->value_count && valid; ++index) {
        const CmHirDeclarationValue *value = &metadata->values[index];
        uint32_t child;
        int has_erased = 0;
        if (value->kind == CM_HIR_DECL_VALUE_FUNCTION) {
            has_erased = contains_erased[value->return_type - 1u] != 0u;
            for (child = 0u; child < value->parameter_count; ++child) {
                if (contains_erased[value->parameter_types[child] - 1u])
                    has_erased = 1;
            }
            if (has_erased && !cm_decl_bounded_free_erased_value(metadata,
                    value, (uint32_t)(index + 1u))) valid = 0;
        } else if (contains_erased[value->declared_type - 1u]) {
            valid = 0;
        }
    }
    for (index = 0u; index < metadata->associated_count && valid; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        uint32_t child;
        /* Slot zero is the separately authenticated shared receiver.  An
         * ERASED return may preserve receiver-driven output elision. */
        for (child = 1u; child < associated->parameter_count; ++child) {
            if (contains_erased[associated->parameter_types[child] - 1u]) {
                if (child != 1u
                    || !cm_decl_bounded_clone_from_erased_source(metadata,
                        associated)) valid = 0;
                break;
            }
        }
    }
    for (index = 0u; index < metadata->item_count && valid; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        uint32_t child;
        if (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
            && contains_erased[item->alias_target_type - 1u]) valid = 0;
        for (child = 0u; child < item->field_count && valid; ++child) {
            if (contains_erased[item->fields[child].type_local - 1u])
                valid = 0;
        }
        if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t variant;
            for (variant = 0u; variant < item->variant_count && valid;
                    ++variant) {
                uint32_t field;
                for (field = 0u;
                        field < item->variants[variant].field_count;
                        ++field) {
                    if (contains_erased[item->variants[variant]
                            .fields[field].type_local - 1u]) {
                        valid = 0;
                        break;
                    }
                }
            }
        }
    }
    for (index = 0u; index < metadata->predicate_count && valid; ++index) {
        const CmHirDeclarationPredicate *predicate =
            &metadata->predicates[index];
        uint32_t child;
        if (contains_erased[predicate->subject_type - 1u]) valid = 0;
        for (child = 0u; child < predicate->argument_count && valid;
                ++child) {
            if (contains_erased[predicate->argument_types[child] - 1u])
                valid = 0;
        }
        for (child = 0u; child < predicate->equality_count && valid;
                ++child) {
            if (contains_erased[predicate->equalities[child]
                    .value_type - 1u]) valid = 0;
        }
    }
    for (index = 0u; index < metadata->trait_count && valid; ++index) {
        const CmHirDeclarationTrait *trait_value = &metadata->traits[index];
        uint32_t child;
        for (child = 0u; child < trait_value->supertrait_count && valid;
                ++child) {
            const CmHirDeclarationSupertrait *supertrait =
                &trait_value->supertraits[child];
            uint32_t argument;
            for (argument = 0u; argument < supertrait->argument_count;
                    ++argument) {
                if (contains_erased[
                        supertrait->argument_types[argument] - 1u]) {
                    valid = 0;
                    break;
                }
            }
        }
    }
    for (index = 0u; index < metadata->outlives_predicate_count && valid;
            ++index) {
        if (contains_erased[metadata->outlives_predicates[index]
                .subject_type - 1u]) valid = 0;
    }
    cm_free(contains_erased);
    return valid;
}

static uint32_t cm_decl_trait_with_lang(
    const CmHirDeclarationMetadata *metadata, const char *lang)
{
    size_t index;
    uint32_t found = 0u;
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirDeclarationTrait *trait_value = &metadata->traits[index];
        if ((trait_value->flags & CM_HIR_DECL_TRAIT_HAS_LANG_ITEM) == 0u
            || !cm_decl_string_is(trait_value->lang_item, lang)) continue;
        if (found != 0u) return 0u;
        found = (uint32_t)(index + 1u);
    }
    return found;
}

static uint32_t cm_decl_associated_with_lang(
    const CmHirDeclarationMetadata *metadata, const char *lang)
{
    size_t index;
    uint32_t found = 0u;
    for (index = 0u; index < metadata->associated_count; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        if ((associated->flags & CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM) == 0u
            || !cm_decl_string_is(associated->lang_item, lang)) continue;
        if (found != 0u) return 0u;
        found = (uint32_t)(index + 1u);
    }
    return found;
}

static int cm_decl_type_is_generic(const CmHirDeclarationMetadata *metadata,
    uint32_t type_local, uint32_t generic_local)
{
    const CmHirDeclarationType *type;
    if (!cm_decl_type_local(metadata, type_local)) return 0;
    type = &metadata->types[type_local - 1u];
    return type->kind == CM_HIR_DECL_TYPE_GENERIC
        && type->generic_local == generic_local;
}

static int cm_decl_type_is_self(const CmHirDeclarationMetadata *metadata,
    uint32_t type_local, uint32_t trait_local)
{
    const CmHirDeclarationType *type;
    if (!cm_decl_type_local(metadata, type_local)) return 0;
    type = &metadata->types[type_local - 1u];
    return type->kind == CM_HIR_DECL_TYPE_SELF
        && type->self_trait_local == trait_local;
}

static int cm_decl_callable_projection(
    const CmHirDeclarationMetadata *metadata, uint32_t type_local,
    uint32_t self_trait, uint32_t fn_once_trait, uint32_t output_associated,
    uint32_t argument_generic)
{
    const CmHirDeclarationType *type;
    if (!cm_decl_type_local(metadata, type_local)) return 0;
    type = &metadata->types[type_local - 1u];
    return type->kind == CM_HIR_DECL_TYPE_PROJECTION
        && cm_decl_type_is_self(metadata, type->projection_self_type,
            self_trait)
        && type->projection_trait_local == fn_once_trait
        && type->projection_associated_local == output_associated
        && type->projection_argument_count == 1u
        && type->projection_argument_types != NULL
        && cm_decl_type_is_generic(metadata,
            type->projection_argument_types[0], argument_generic);
}

static int cm_decl_callable_method(const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationAssociatedItem *method, uint32_t parent_trait,
    uint32_t argument_generic, uint32_t fn_once_trait,
    uint32_t output_associated, int is_mutable)
{
    const CmHirDeclarationType *receiver;
    if (method->kind != CM_HIR_DECL_ASSOCIATED_METHOD
        || method->parent_local != parent_trait
        || method->receiver != (is_mutable
            ? CM_HIR_DECL_RECEIVER_REF_MUTABLE
            : CM_HIR_DECL_RECEIVER_VALUE)
        || method->parameter_count != 2u
        || method->parameter_types == NULL
        || !cm_decl_string_is(method->abi, "rust-call")
        || method->safety != CM_HIR_DECL_SAFETY_SAFE
        || method->has_default_body != 0u
        || method->predicate_start != 0u || method->predicate_count != 0u
        || !cm_decl_type_is_generic(metadata, method->parameter_types[1],
            argument_generic)
        || !cm_decl_callable_projection(metadata, method->return_type,
            parent_trait, fn_once_trait, output_associated,
            argument_generic)) return 0;
    if (!is_mutable)
        return cm_decl_type_is_self(metadata, method->parameter_types[0],
            parent_trait);
    receiver = &metadata->types[method->parameter_types[0] - 1u];
    return receiver->kind == CM_HIR_DECL_TYPE_REFERENCE
        && receiver->region.kind == CM_HIR_DECL_REGION_ERASED
        && receiver->mutability == CM_HIR_DECL_MUTABLE
        && cm_decl_type_is_self(metadata, receiver->child_type,
            parent_trait);
}

static int cm_decl_validate_supertrait_acyclic(
    const CmHirDeclarationMetadata *metadata)
{
    uint32_t *indegree;
    uint32_t *queue;
    size_t index;
    size_t cursor = 0u;
    size_t count = 0u;
    size_t visited = 0u;
    indegree = metadata->trait_count == 0u ? NULL
        : (uint32_t *)cm_alloc_zeroed(metadata->trait_count,
            sizeof(*indegree));
    queue = metadata->trait_count == 0u ? NULL
        : (uint32_t *)cm_alloc(metadata->trait_count * sizeof(*queue));
    for (index = 0u; index < metadata->trait_count; ++index) {
        uint32_t child;
        const CmHirDeclarationTrait *trait_value = &metadata->traits[index];
        for (child = 0u; child < trait_value->supertrait_count; ++child) {
            uint32_t target = trait_value->supertraits[child].trait_local;
            if (indegree[target - 1u] == UINT32_MAX) {
                cm_free(queue);
                cm_free(indegree);
                return 0;
            }
            indegree[target - 1u] += 1u;
        }
    }
    for (index = 0u; index < metadata->trait_count; ++index)
        if (indegree[index] == 0u) queue[count++] = (uint32_t)(index + 1u);
    while (cursor < count) {
        uint32_t local = queue[cursor++];
        const CmHirDeclarationTrait *trait_value =
            &metadata->traits[local - 1u];
        uint32_t child;
        visited += 1u;
        for (child = 0u; child < trait_value->supertrait_count; ++child) {
            uint32_t target = trait_value->supertraits[child].trait_local;
            indegree[target - 1u] -= 1u;
            if (indegree[target - 1u] == 0u) queue[count++] = target;
        }
    }
    cm_free(queue);
    cm_free(indegree);
    return visited == metadata->trait_count;
}

/*
 * The new v3.0 slots deliberately authenticate one complete callable-trait
 * closure.  Keeping this profile exact prevents the latent supertrait,
 * associated TYPE, projection, and rust-call fields from becoming a general
 * promise that current consumers cannot uphold.
 */
static int cm_decl_validate_callable_profile(
    const CmHirDeclarationMetadata *metadata)
{
    const uint8_t tuple_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
        | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
    const uint8_t callable_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_RUSTC_PAREN_SUGAR
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL;
    uint32_t tuple_trait = cm_decl_trait_with_lang(metadata, "tuple_trait");
    uint32_t fn_mut_trait = cm_decl_trait_with_lang(metadata, "fn_mut");
    uint32_t fn_once_trait = cm_decl_trait_with_lang(metadata, "fn_once");
    uint32_t output_associated = cm_decl_associated_with_lang(metadata,
        "fn_once_output");
    size_t index;
    if (!cm_decl_validate_supertrait_acyclic(metadata)) return 0;
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirDeclarationTrait *trait_value = &metadata->traits[index];
        uint32_t local = (uint32_t)(index + 1u);
        if ((trait_value->flags & CM_HIR_DECL_TRAIT_HAS_LANG_ITEM) == 0u) {
            if (trait_value->supertrait_count != 0u
                || trait_value->compiler_flags != 0u
                || (trait_value->flags & (uint8_t)~
                    CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM) != 0u)
                return 0;
            continue;
        }
        if (local == tuple_trait) {
            if (trait_value->flags != tuple_flags
                || trait_value->compiler_flags != 0u
                || trait_value->safety != CM_HIR_DECL_SAFETY_SAFE
                || trait_value->generic_start != 0u
                || trait_value->generic_count != 0u
                || trait_value->predicate_start != 0u
                || trait_value->predicate_count != 0u
                || trait_value->outlives_start != 0u
                || trait_value->outlives_count != 0u
                || trait_value->supertrait_count != 0u
                || trait_value->associated_start != 0u
                || trait_value->associated_count != 0u) return 0;
        } else if (local == fn_once_trait || local == fn_mut_trait) {
            const CmHirDeclarationGeneric *generic;
            const CmHirDeclarationPredicate *predicate;
            if (trait_value->flags != callable_flags
                || trait_value->compiler_flags != 0u
                || trait_value->safety != CM_HIR_DECL_SAFETY_SAFE
                || trait_value->generic_count != 1u
                || trait_value->generic_start == 0u
                || trait_value->predicate_count != 1u
                || trait_value->predicate_start == 0u
                || tuple_trait == 0u) return 0;
            generic = &metadata->generics[trait_value->generic_start - 1u];
            predicate = &metadata->predicates[
                trait_value->predicate_start - 1u];
            if (generic->owner_kind != CM_HIR_DECL_GENERIC_NOMINAL
                || generic->owner_local != local || generic->index != 0u
                || generic->kind != CM_HIR_DECL_GENERIC_TYPE
                || generic->is_relaxed_sized != 0u
                || generic->has_default != 0u
                || predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
                || predicate->owner_nominal != local
                || predicate->ordinal != 0u
                || !cm_decl_type_is_generic(metadata,
                    predicate->subject_type, trait_value->generic_start)
                || predicate->trait_local != tuple_trait
                || predicate->argument_count != 0u
                || predicate->equality_count != 0u) return 0;
            if (local == fn_once_trait) {
                if (trait_value->supertrait_count != 0u
                    || trait_value->associated_count != 2u
                    || output_associated == 0u
                    || output_associated != trait_value->associated_start
                    || metadata->associated_items[output_associated - 1u]
                        .parent_local != local
                    || metadata->associated_items[output_associated - 1u]
                        .kind != CM_HIR_DECL_ASSOCIATED_TYPE
                    || metadata->associated_items[output_associated - 1u]
                        .source_ordinal
                        >= metadata->associated_items[
                            output_associated].source_ordinal
                    || !cm_decl_callable_method(metadata,
                        &metadata->associated_items[output_associated], local,
                        trait_value->generic_start, fn_once_trait,
                        output_associated, 0)) return 0;
            } else {
                const CmHirDeclarationSupertrait *supertrait;
                if (fn_once_trait == 0u
                    || trait_value->supertrait_count != 1u
                    || trait_value->associated_count != 1u) return 0;
                supertrait = &trait_value->supertraits[0];
                if (supertrait->trait_local != fn_once_trait
                    || supertrait->argument_count != 1u
                    || supertrait->argument_types == NULL
                    || !cm_decl_type_is_generic(metadata,
                        supertrait->argument_types[0],
                        trait_value->generic_start)
                    || !cm_decl_callable_method(metadata,
                        &metadata->associated_items[
                            trait_value->associated_start - 1u], local,
                        trait_value->generic_start, fn_once_trait,
                        output_associated, 1)) return 0;
            }
        } else if (cm_decl_string_is(trait_value->lang_item, "clone")
                || cm_decl_string_is(trait_value->lang_item, "destruct")
                || cm_decl_string_is(trait_value->lang_item, "meta_sized")
                || cm_decl_string_is(trait_value->lang_item,
                    "pointee_sized")
                || cm_decl_string_is(trait_value->lang_item, "sized")) {
            continue;
        } else {
            return 0;
        }
    }
    for (index = 0u; index < metadata->associated_count; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_TYPE
            && (uint32_t)(index + 1u) != output_associated) return 0;
        if (cm_decl_string_is(associated->abi, "rust-call")
            && associated->parent_local != fn_once_trait
            && associated->parent_local != fn_mut_trait) return 0;
    }
    for (index = 0u; index < metadata->type_count; ++index) {
        const CmHirDeclarationType *type = &metadata->types[index];
        if (type->kind != CM_HIR_DECL_TYPE_PROJECTION) continue;
        if (output_associated == 0u
            || type->projection_associated_local != output_associated
            || type->projection_trait_local != fn_once_trait
            || type->projection_argument_count != 1u) return 0;
    }
    return 1;
}

static int cm_decl_zero_arg_supertrait_is(
    const CmHirDeclarationTrait *trait_value, uint32_t target_local)
{
    return trait_value->supertrait_count == 1u
        && trait_value->supertraits != NULL
        && trait_value->supertraits[0].modifier
            == CM_HIR_DECL_SUPERTRAIT_REQUIRED
        && trait_value->supertraits[0].trait_local == target_local
        && trait_value->supertraits[0].argument_count == 0u
        && trait_value->supertraits[0].argument_types == NULL;
}

static int cm_decl_clone_marker_trait_common(
    const CmHirDeclarationTrait *trait_value, uint8_t flags,
    uint16_t compiler_flags)
{
    return trait_value->visibility.kind == CM_HIR_DECL_VISIBILITY_PUBLIC
        && trait_value->visibility.restriction_module == 0u
        && trait_value->safety == CM_HIR_DECL_SAFETY_SAFE
        && trait_value->flags == flags
        && trait_value->compiler_flags == compiler_flags
        && trait_value->generic_start == 0u
        && trait_value->generic_count == 0u
        && trait_value->predicate_start == 0u
        && trait_value->predicate_count == 0u
        && trait_value->outlives_start == 0u
        && trait_value->outlives_count == 0u;
}

static int cm_decl_clone_method_common(
    const CmHirDeclarationAssociatedItem *method, uint32_t parent_local,
    uint8_t receiver, uint32_t parameter_count, int has_default_body)
{
    return method->kind == CM_HIR_DECL_ASSOCIATED_METHOD
        && method->parent_kind == CM_HIR_DECL_ASSOCIATED_PARENT_NOMINAL
        && method->parent_local == parent_local
        && method->implemented_associated_local == 0u
        && method->visibility.kind == CM_HIR_DECL_VISIBILITY_PRIVATE
        && method->visibility.restriction_module == 0u
        && method->is_specializable == 0u
        && method->generic_start == 0u && method->generic_count == 0u
        && method->receiver == receiver
        && method->parameter_count == parameter_count
        && method->parameter_types != NULL
        && cm_decl_string_is(method->abi, "Rust")
        && method->safety == CM_HIR_DECL_SAFETY_SAFE
        && method->is_const == 0u && method->is_async == 0u
        && method->is_variadic == 0u
        && method->has_default_body == (uint8_t)has_default_body;
}

static int cm_decl_reference_to_self_is(
    const CmHirDeclarationMetadata *metadata, uint32_t type_local,
    uint32_t trait_local, uint8_t mutability)
{
    const CmHirDeclarationType *reference;
    if (!cm_decl_type_local(metadata, type_local)) return 0;
    reference = &metadata->types[type_local - 1u];
    return reference->kind == CM_HIR_DECL_TYPE_REFERENCE
        && reference->region.kind == CM_HIR_DECL_REGION_ERASED
        && reference->region.generic_local == 0u
        && reference->region.binder_index == 0u
        && reference->mutability == mutability
        && cm_decl_type_is_self(metadata, reference->child_type,
            trait_local);
}

/*
 * Exact reachable declaration closure for `T: Clone`: the const Clone trait,
 * its two methods, Sized hierarchy, and the const-if-const Destruct predicate
 * on clone_from.  The semantic lang identities are the authority; ordinary
 * names are checked only for the two associated source declarations.
 */
static int cm_decl_validate_clone_profile(
    const CmHirDeclarationMetadata *metadata)
{
    const uint8_t clone_flags = CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM
        | CM_HIR_DECL_TRAIT_HAS_LANG_ITEM | CM_HIR_DECL_TRAIT_IS_CONST;
    const uint8_t marker_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_FUNDAMENTAL
        | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
        | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
    const uint8_t destruct_flags = CM_HIR_DECL_TRAIT_HAS_LANG_ITEM
        | CM_HIR_DECL_TRAIT_IS_CONST
        | CM_HIR_DECL_TRAIT_DENY_EXPLICIT_IMPL
        | CM_HIR_DECL_TRAIT_DO_NOT_IMPLEMENT_VIA_OBJECT;
    const uint16_t marker_compiler_flags =
        CM_HIR_DECL_TRAIT_COMPILER_SPECIALIZATION
        | CM_HIR_DECL_TRAIT_COMPILER_COINDUCTIVE;
    uint32_t clone_local = cm_decl_trait_with_lang(metadata, "clone");
    uint32_t destruct_local = cm_decl_trait_with_lang(metadata, "destruct");
    uint32_t meta_local = cm_decl_trait_with_lang(metadata, "meta_sized");
    uint32_t pointee_local = cm_decl_trait_with_lang(metadata,
        "pointee_sized");
    uint32_t sized_local = cm_decl_trait_with_lang(metadata, "sized");
    uint32_t clone_method_local = cm_decl_associated_with_lang(metadata,
        "clone_fn");
    const CmHirDeclarationTrait *clone_trait;
    const CmHirDeclarationTrait *destruct_trait;
    const CmHirDeclarationTrait *meta_trait;
    const CmHirDeclarationTrait *pointee_trait;
    const CmHirDeclarationTrait *sized_trait;
    const CmHirDeclarationAssociatedItem *clone_method;
    const CmHirDeclarationAssociatedItem *clone_from;
    const CmHirDeclarationPredicate *predicate;
    const CmHirDeclarationType *unit;
    size_t index;
    int any = clone_local != 0u || destruct_local != 0u || meta_local != 0u
        || pointee_local != 0u || sized_local != 0u
        || clone_method_local != 0u;
    if (!any) {
        for (index = 0u; index < metadata->trait_count; ++index)
            if (metadata->traits[index].compiler_flags != 0u) return 0;
        for (index = 0u; index < metadata->associated_count; ++index) {
            const CmHirDeclarationAssociatedItem *associated =
                &metadata->associated_items[index];
            if (associated->kind == CM_HIR_DECL_ASSOCIATED_METHOD
                && associated->flags != 0u) return 0;
        }
        for (index = 0u; index < metadata->predicate_count; ++index)
            if (metadata->predicates[index].modifier
                    != CM_HIR_DECL_PREDICATE_REQUIRED) return 0;
        return 1;
    }
    if (clone_local == 0u || destruct_local == 0u || meta_local == 0u
        || pointee_local == 0u || sized_local == 0u
        || clone_method_local == 0u) return 0;
    clone_trait = &metadata->traits[clone_local - 1u];
    destruct_trait = &metadata->traits[destruct_local - 1u];
    meta_trait = &metadata->traits[meta_local - 1u];
    pointee_trait = &metadata->traits[pointee_local - 1u];
    sized_trait = &metadata->traits[sized_local - 1u];
    if (!cm_decl_clone_marker_trait_common(clone_trait, clone_flags,
            CM_HIR_DECL_TRAIT_COMPILER_TRIVIAL_FIELD_READS)
        || !cm_decl_string_is(clone_trait->diagnostic_item, "Clone")
        || clone_trait->associated_count != 2u
        || clone_trait->associated_start == 0u
        || clone_method_local != clone_trait->associated_start
        || !cm_decl_zero_arg_supertrait_is(clone_trait, sized_local)
        || !cm_decl_clone_marker_trait_common(destruct_trait,
            destruct_flags, UINT16_C(0))
        || destruct_trait->associated_start != 0u
        || destruct_trait->associated_count != 0u
        || destruct_trait->supertrait_count != 0u
        || !cm_decl_clone_marker_trait_common(meta_trait, marker_flags,
            marker_compiler_flags)
        || meta_trait->associated_start != 0u
        || meta_trait->associated_count != 0u
        || !cm_decl_zero_arg_supertrait_is(meta_trait, pointee_local)
        || !cm_decl_clone_marker_trait_common(pointee_trait, marker_flags,
            marker_compiler_flags)
        || pointee_trait->associated_start != 0u
        || pointee_trait->associated_count != 0u
        || pointee_trait->supertrait_count != 0u
        || !cm_decl_clone_marker_trait_common(sized_trait, marker_flags,
            marker_compiler_flags)
        || sized_trait->associated_start != 0u
        || sized_trait->associated_count != 0u
        || !cm_decl_zero_arg_supertrait_is(sized_trait, meta_local)) return 0;
    clone_method = &metadata->associated_items[clone_method_local - 1u];
    clone_from = &metadata->associated_items[clone_method_local];
    if (!cm_decl_clone_method_common(clone_method, clone_local,
            CM_HIR_DECL_RECEIVER_REF_SHARED, 1u, 0)
        || !cm_decl_string_is(clone_method->name, "clone")
        || clone_method->flags != CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM
        || !cm_decl_string_is(clone_method->lang_item, "clone_fn")
        || clone_method->predicate_start != 0u
        || clone_method->predicate_count != 0u
        || !cm_decl_reference_to_self_is(metadata,
            clone_method->parameter_types[0], clone_local,
            CM_HIR_DECL_IMMUTABLE)
        || !cm_decl_type_is_self(metadata, clone_method->return_type,
            clone_local)
        || !cm_decl_clone_method_common(clone_from, clone_local,
            CM_HIR_DECL_RECEIVER_REF_MUTABLE, 2u, 1)
        || !cm_decl_string_is(clone_from->name, "clone_from")
        || clone_from->flags != 0u
        || clone_from->predicate_count != 1u
        || clone_from->predicate_start == 0u
        || !cm_decl_reference_to_self_is(metadata,
            clone_from->parameter_types[0], clone_local,
            CM_HIR_DECL_MUTABLE)
        || !cm_decl_reference_to_self_is(metadata,
            clone_from->parameter_types[1], clone_local,
            CM_HIR_DECL_IMMUTABLE)) return 0;
    unit = &metadata->types[clone_from->return_type - 1u];
    predicate = &metadata->predicates[clone_from->predicate_start - 1u];
    if (unit->kind != CM_HIR_DECL_TYPE_PRIMITIVE
        || unit->primitive != CM_HIR_DECL_PRIMITIVE_UNIT
        || predicate->owner_kind
            != CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
        || predicate->owner_associated != clone_method_local + 1u
        || predicate->ordinal != 0u
        || !cm_decl_type_is_self(metadata, predicate->subject_type,
            clone_local)
        || predicate->trait_local != destruct_local
        || predicate->argument_count != 0u
        || predicate->equality_count != 0u
        || predicate->modifier
            != CM_HIR_DECL_PREDICATE_CONST_IF_CONST) return 0;
    for (index = 0u; index < metadata->associated_count; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_METHOD
            && associated->flags != 0u
            && (uint32_t)(index + 1u) != clone_method_local) return 0;
    }
    for (index = 0u; index < metadata->predicate_count; ++index) {
        const CmHirDeclarationPredicate *candidate =
            &metadata->predicates[index];
        if (candidate->modifier != CM_HIR_DECL_PREDICATE_REQUIRED
            && candidate != predicate) return 0;
    }
    return 1;
}

static int cm_decl_repeat_value_profile(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationValue *value, uint32_t value_local,
    uint32_t clone_local)
{
    const CmHirDeclarationGeneric *type_generic;
    const CmHirDeclarationGeneric *const_generic;
    const CmHirDeclarationType *const_type;
    const CmHirDeclarationType *result;
    const CmHirDeclarationPredicate *predicate;
    if (clone_local == 0u || value->generic_start == 0u
        || value->generic_count != 2u || value->predicate_start == 0u
        || value->predicate_count != 1u || value->parameter_count != 1u
        || value->parameter_types == NULL || value->has_body != 1u
        || value->is_const != 0u) return 0;
    type_generic = &metadata->generics[value->generic_start - 1u];
    const_generic = &metadata->generics[value->generic_start];
    const_type = &metadata->types[const_generic->declared_type - 1u];
    result = &metadata->types[value->return_type - 1u];
    predicate = &metadata->predicates[value->predicate_start - 1u];
    return type_generic->owner_kind == CM_HIR_DECL_GENERIC_VALUE
        && type_generic->owner_local == value_local
        && type_generic->index == 0u
        && type_generic->kind == CM_HIR_DECL_GENERIC_TYPE
        && type_generic->is_relaxed_sized == 0u
        && type_generic->has_default == 0u
        && type_generic->declared_type == 0u
        && const_generic->owner_kind == CM_HIR_DECL_GENERIC_VALUE
        && const_generic->owner_local == value_local
        && const_generic->index == 1u
        && const_generic->kind == CM_HIR_DECL_GENERIC_CONST
        && const_generic->is_relaxed_sized == 0u
        && const_generic->has_default == 0u
        && const_type->kind == CM_HIR_DECL_TYPE_PRIMITIVE
        && const_type->primitive == CM_HIR_DECL_PRIMITIVE_USIZE
        && cm_decl_type_is_generic(metadata, value->parameter_types[0],
            value->generic_start)
        && result->kind == CM_HIR_DECL_TYPE_ARRAY
        && cm_decl_type_is_generic(metadata, result->child_type,
            value->generic_start)
        && result->array_length_kind
            == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER
        && result->array_length_generic_local == value->generic_start + 1u
        && predicate->owner_kind == CM_HIR_DECL_PREDICATE_OWNER_VALUE
        && predicate->owner_value == value_local
        && predicate->ordinal == 0u
        && cm_decl_type_is_generic(metadata, predicate->subject_type,
            value->generic_start)
        && predicate->trait_local == clone_local
        && predicate->argument_count == 0u
        && predicate->equality_count == 0u
        && predicate->modifier == CM_HIR_DECL_PREDICATE_REQUIRED;
}

static int cm_decl_validate_from_fn_profile(
    const CmHirDeclarationMetadata *metadata)
{
    uint32_t clone_trait = cm_decl_trait_with_lang(metadata, "clone");
    uint32_t fn_mut_trait = cm_decl_trait_with_lang(metadata, "fn_mut");
    uint32_t output_associated = cm_decl_associated_with_lang(metadata,
        "fn_once_output");
    size_t index;
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirDeclarationValue *value = &metadata->values[index];
        const CmHirDeclarationGeneric *type_generic;
        const CmHirDeclarationGeneric *const_generic;
        const CmHirDeclarationGeneric *function_generic;
        const CmHirDeclarationType *result;
        const CmHirDeclarationPredicate *predicate;
        const CmHirDeclarationType *tuple;
        uint32_t child;
        int uses_new_profile = 0;
        if (value->kind != CM_HIR_DECL_VALUE_FUNCTION
            || value->generic_count == 0u) continue;
        if (value->generic_start == 0u) return 0;
        type_generic = &metadata->generics[value->generic_start - 1u];
        for (child = 0u; child < value->generic_count; ++child) {
            if (metadata->generics[
                    (size_t)value->generic_start + child - 1u].kind
                    == CM_HIR_DECL_GENERIC_CONST)
                uses_new_profile = 1;
        }
        for (child = 0u; child < value->predicate_count; ++child) {
            const CmHirDeclarationPredicate *candidate =
                &metadata->predicates[
                    (size_t)value->predicate_start + child - 1u];
            uint32_t argument;
            if (candidate->equality_count != 0u) uses_new_profile = 1;
            for (argument = 0u; argument < candidate->argument_count;
                    ++argument) {
                const CmHirDeclarationType *argument_type =
                    &metadata->types[
                        candidate->argument_types[argument] - 1u];
                if (argument_type->kind != CM_HIR_DECL_TYPE_PRIMITIVE)
                    uses_new_profile = 1;
            }
        }
        if (!uses_new_profile) continue;
        if (cm_decl_repeat_value_profile(metadata, value,
                (uint32_t)(index + 1u), clone_trait)) continue;
        if (value->generic_count != 3u || value->parameter_count != 1u
            || value->parameter_types == NULL
            || value->predicate_count != 1u || value->predicate_start == 0u
            || value->has_body != 1u || value->is_const != 0u
            || fn_mut_trait == 0u || output_associated == 0u) return 0;
        const_generic = &metadata->generics[value->generic_start];
        function_generic = &metadata->generics[value->generic_start + 1u];
        result = &metadata->types[value->return_type - 1u];
        predicate = &metadata->predicates[value->predicate_start - 1u];
        if (type_generic->kind != CM_HIR_DECL_GENERIC_TYPE
            || const_generic->kind != CM_HIR_DECL_GENERIC_CONST
            || function_generic->kind != CM_HIR_DECL_GENERIC_TYPE
            || type_generic->index != 0u || const_generic->index != 1u
            || function_generic->index != 2u
            || type_generic->owner_kind != CM_HIR_DECL_GENERIC_VALUE
            || const_generic->owner_kind != CM_HIR_DECL_GENERIC_VALUE
            || function_generic->owner_kind != CM_HIR_DECL_GENERIC_VALUE
            || type_generic->owner_local != (uint32_t)(index + 1u)
            || const_generic->owner_local != (uint32_t)(index + 1u)
            || function_generic->owner_local != (uint32_t)(index + 1u)
            || type_generic->is_relaxed_sized != 0u
            || const_generic->is_relaxed_sized != 0u
            || function_generic->is_relaxed_sized != 0u
            || type_generic->has_default != 0u
            || const_generic->has_default != 0u
            || function_generic->has_default != 0u
            || !cm_decl_type_is_generic(metadata,
                value->parameter_types[0], value->generic_start + 2u)
            || result->kind != CM_HIR_DECL_TYPE_ARRAY
            || !cm_decl_type_is_generic(metadata, result->child_type,
                value->generic_start)
            || result->array_length_kind
                != CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER
            || result->array_length_generic_local
                != value->generic_start + 1u
            || predicate->owner_kind
                != CM_HIR_DECL_PREDICATE_OWNER_VALUE
            || predicate->owner_value != (uint32_t)(index + 1u)
            || predicate->ordinal != 0u
            || !cm_decl_type_is_generic(metadata, predicate->subject_type,
                value->generic_start + 2u)
            || predicate->trait_local != fn_mut_trait
            || predicate->argument_count != 1u
            || predicate->argument_types == NULL
            || predicate->equality_count != 1u
            || predicate->equalities == NULL
            || predicate->equalities[0].associated_local
                != output_associated
            || !cm_decl_type_is_generic(metadata,
                predicate->equalities[0].value_type,
                value->generic_start)) return 0;
        tuple = &metadata->types[predicate->argument_types[0] - 1u];
        if (tuple->kind != CM_HIR_DECL_TYPE_TUPLE
            || tuple->element_count != 1u || tuple->element_types == NULL
            || metadata->types[tuple->element_types[0] - 1u].kind
                != CM_HIR_DECL_TYPE_PRIMITIVE
            || metadata->types[tuple->element_types[0] - 1u].primitive
                != CM_HIR_DECL_PRIMITIVE_USIZE) return 0;
    }
    return 1;
}

static int cm_decl_namespace_compare(
    const CmHirDeclarationNamespaceEntry *left,
    const CmHirDeclarationNamespaceEntry *right)
{
    int order;
    if (left->owner_module < right->owner_module) return -1;
    if (left->owner_module > right->owner_module) return 1;
    if (left->namespace_kind < right->namespace_kind) return -1;
    if (left->namespace_kind > right->namespace_kind) return 1;
    order = cm_decl_string_compare(left->name, right->name);
    if (order != 0) return order;
    return left->export_ordinal < right->export_ordinal ? -1
        : left->export_ordinal > right->export_ordinal;
}

static const CmHirDeclarationVariant *cm_decl_variant_local(
    const CmHirDeclarationMetadata *metadata, uint32_t local,
    const CmHirDeclarationItem **owner_out)
{
    size_t index;
    uint32_t remaining;
    if (local == 0u) return NULL;
    remaining = local;
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        if (item->kind != CM_HIR_DECL_ITEM_ENUM) continue;
        if (remaining <= item->variant_count) {
            if (owner_out != NULL) *owner_out = item;
            return &item->variants[remaining - 1u];
        }
        remaining -= item->variant_count;
    }
    return NULL;
}

static const CmHirDeclarationNamespaceEntry *cm_decl_namespace_find(
    const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationNamespaceEntry *key)
{
    size_t first = 0u;
    size_t count = metadata->namespace_count;
    while (count != 0u) {
        size_t step = count / 2u;
        size_t current = first + step;
        int order = cm_decl_namespace_compare(
            &metadata->namespace_entries[current], key);
        if (order < 0) {
            first = current + 1u;
            count -= step + 1u;
        } else if (order > 0) {
            count = step;
        } else {
            return &metadata->namespace_entries[current];
        }
    }
    return NULL;
}

static void cm_decl_reach_type(const CmHirDeclarationMetadata *metadata,
    unsigned char *seen, uint32_t *queue, size_t *queue_count,
    uint32_t local)
{
    if (local == 0u || (size_t)local > metadata->type_count
        || seen[local - 1u]) return;
    seen[local - 1u] = 1u;
    queue[*queue_count] = local;
    *queue_count += 1u;
}

static void cm_decl_reach_item(const CmHirDeclarationMetadata *metadata,
    unsigned char *seen, uint32_t *queue, size_t *queue_count,
    uint32_t local)
{
    if (local == 0u || (size_t)local > metadata->item_count
        || seen[local - 1u]) return;
    seen[local - 1u] = 1u;
    queue[*queue_count] = local;
    *queue_count += 1u;
}

static void cm_decl_reach_trait(const CmHirDeclarationMetadata *metadata,
    unsigned char *seen, uint32_t *queue, size_t *queue_count,
    uint32_t local)
{
    if (local == 0u || (size_t)local > metadata->trait_count
        || seen[local - 1u]) return;
    seen[local - 1u] = 1u;
    queue[*queue_count] = local;
    *queue_count += 1u;
}

/*
 * Nonpublic ITEMs and traits are dependency closure, not independent artifact
 * roots. Traverse only from public declarations and reject every orphan.
 */
static int cm_decl_validate_private_item_reachability(
    const CmHirDeclarationMetadata *metadata)
{
    unsigned char *item_seen;
    unsigned char *trait_seen;
    unsigned char *type_seen;
    uint32_t *item_queue;
    uint32_t *trait_queue;
    uint32_t *type_queue;
    size_t item_count;
    size_t trait_count;
    size_t type_count;
    size_t item_cursor;
    size_t trait_cursor;
    size_t type_cursor;
    size_t index;
    item_seen = metadata->item_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->item_count,
            sizeof(*item_seen));
    type_seen = metadata->type_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->type_count,
            sizeof(*type_seen));
    trait_seen = metadata->trait_count == 0u ? NULL
        : (unsigned char *)cm_alloc_zeroed(metadata->trait_count,
            sizeof(*trait_seen));
    item_queue = metadata->item_count == 0u ? NULL
        : (uint32_t *)cm_alloc_zeroed(metadata->item_count,
            sizeof(*item_queue));
    type_queue = metadata->type_count == 0u ? NULL
        : (uint32_t *)cm_alloc_zeroed(metadata->type_count,
            sizeof(*type_queue));
    trait_queue = metadata->trait_count == 0u ? NULL
        : (uint32_t *)cm_alloc_zeroed(metadata->trait_count,
            sizeof(*trait_queue));
    item_count = 0u;
    trait_count = 0u;
    type_count = 0u;
    for (index = 0u; index < metadata->item_count; ++index) {
        if (metadata->items[index].visibility.kind
                == CM_HIR_DECL_VISIBILITY_PUBLIC)
            cm_decl_reach_item(metadata, item_seen, item_queue, &item_count,
                (uint32_t)(index + 1u));
    }
    for (index = 0u; index < metadata->trait_count; ++index) {
        if (metadata->traits[index].visibility.kind
                == CM_HIR_DECL_VISIBILITY_PUBLIC)
            cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                &trait_count, (uint32_t)(index + 1u));
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirDeclarationValue *value = &metadata->values[index];
        uint32_t child;
        if (value->kind == CM_HIR_DECL_VALUE_FUNCTION) {
            cm_decl_reach_type(metadata, type_seen, type_queue, &type_count,
                value->return_type);
            for (child = 0u; child < value->parameter_count; ++child)
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, value->parameter_types[child]);
        } else {
            cm_decl_reach_type(metadata, type_seen, type_queue, &type_count,
                value->declared_type);
        }
        for (child = 0u; child < value->predicate_count; ++child) {
            const CmHirDeclarationPredicate *predicate =
                &metadata->predicates[
                    (size_t)value->predicate_start + child - 1u];
            uint32_t argument;
            cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                &trait_count, predicate->trait_local);
            cm_decl_reach_type(metadata, type_seen, type_queue, &type_count,
                predicate->subject_type);
            for (argument = 0u; argument < predicate->argument_count;
                    ++argument)
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, predicate->argument_types[argument]);
            for (argument = 0u; argument < predicate->equality_count;
                    ++argument) {
                const CmHirDeclarationPredicateEquality *equality =
                    &predicate->equalities[argument];
                cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                    &trait_count, metadata->associated_items[
                        equality->associated_local - 1u].parent_local);
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, equality->value_type);
            }
        }
    }
    item_cursor = 0u;
    trait_cursor = 0u;
    type_cursor = 0u;
    while (item_cursor < item_count || trait_cursor < trait_count
            || type_cursor < type_count) {
        while (item_cursor < item_count) {
            const CmHirDeclarationItem *item = &metadata->items[
                item_queue[item_cursor] - 1u];
            uint32_t child;
            item_cursor += 1u;
            for (child = 0u; child < item->generic_count; ++child) {
                const CmHirDeclarationGeneric *generic =
                    &metadata->generics[
                        (size_t)item->generic_start + child - 1u];
                if (generic->kind == CM_HIR_DECL_GENERIC_CONST)
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, generic->declared_type);
            }
            if (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS)
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, item->alias_target_type);
            for (child = 0u; child < item->field_count; ++child)
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, item->fields[child].type_local);
            if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
                uint32_t variant;
                for (variant = 0u; variant < item->variant_count; ++variant) {
                    uint32_t field;
                    for (field = 0u;
                            field < item->variants[variant].field_count;
                            ++field)
                        cm_decl_reach_type(metadata, type_seen, type_queue,
                            &type_count, item->variants[variant]
                                .fields[field].type_local);
                }
            }
            for (child = 0u; child < item->predicate_count; ++child) {
                const CmHirDeclarationPredicate *predicate =
                    &metadata->predicates[
                        (size_t)item->predicate_start + child - 1u];
                uint32_t argument;
                cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                    &trait_count, predicate->trait_local);
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, predicate->subject_type);
                for (argument = 0u; argument < predicate->argument_count;
                        ++argument)
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, predicate->argument_types[argument]);
                for (argument = 0u; argument < predicate->equality_count;
                        ++argument) {
                    const CmHirDeclarationPredicateEquality *equality =
                        &predicate->equalities[argument];
                    cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                        &trait_count, metadata->associated_items[
                            equality->associated_local - 1u].parent_local);
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, equality->value_type);
                }
            }
        }
        while (trait_cursor < trait_count) {
            const CmHirDeclarationTrait *trait_value = &metadata->traits[
                trait_queue[trait_cursor] - 1u];
            uint32_t child;
            trait_cursor += 1u;
            for (child = 0u; child < trait_value->supertrait_count;
                    ++child) {
                const CmHirDeclarationSupertrait *supertrait =
                    &trait_value->supertraits[child];
                uint32_t argument;
                cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                    &trait_count, supertrait->trait_local);
                for (argument = 0u;
                        argument < supertrait->argument_count; ++argument)
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, supertrait->argument_types[argument]);
            }
            for (child = 0u; child < trait_value->associated_count; ++child) {
                const CmHirDeclarationAssociatedItem *associated =
                    &metadata->associated_items[
                        (size_t)trait_value->associated_start + child - 1u];
                uint32_t parameter;
                uint32_t predicate_index;
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, associated->return_type);
                for (parameter = 0u;
                        parameter < associated->parameter_count; ++parameter)
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, associated->parameter_types[parameter]);
                for (predicate_index = 0u;
                        predicate_index < associated->predicate_count;
                        ++predicate_index) {
                    const CmHirDeclarationPredicate *predicate =
                        &metadata->predicates[
                            (size_t)associated->predicate_start
                            + predicate_index - 1u];
                    uint32_t argument;
                    cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                        &trait_count, predicate->trait_local);
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, predicate->subject_type);
                    for (argument = 0u;
                            argument < predicate->argument_count; ++argument)
                        cm_decl_reach_type(metadata, type_seen, type_queue,
                            &type_count, predicate->argument_types[argument]);
                    for (argument = 0u;
                            argument < predicate->equality_count; ++argument) {
                        const CmHirDeclarationPredicateEquality *equality =
                            &predicate->equalities[argument];
                        cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                            &trait_count, metadata->associated_items[
                                equality->associated_local - 1u]
                                .parent_local);
                        cm_decl_reach_type(metadata, type_seen, type_queue,
                            &type_count, equality->value_type);
                    }
                }
            }
            for (child = 0u; child < trait_value->outlives_count; ++child)
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, metadata->outlives_predicates[
                        (size_t)trait_value->outlives_start + child - 1u]
                        .subject_type);
        }
        while (type_cursor < type_count) {
            const CmHirDeclarationType *type = &metadata->types[
                type_queue[type_cursor] - 1u];
            uint32_t child;
            type_cursor += 1u;
            if (type->kind == CM_HIR_DECL_TYPE_NAMED_ADT
                || type->kind == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION)
                cm_decl_reach_item(metadata, item_seen, item_queue,
                    &item_count, type->item_local);
            if (type->kind == CM_HIR_DECL_TYPE_SLICE
                || type->kind == CM_HIR_DECL_TYPE_RAW_POINTER
                || type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, type->child_type);
            } else if (type->kind
                    == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
                for (child = 0u; child < type->argument_count; ++child)
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, type->argument_types[child]);
            } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
                for (child = 0u; child < type->element_count; ++child)
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, type->element_types[child]);
            } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY) {
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, type->child_type);
                if (type->array_length_kind
                        == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER) {
                    const CmHirDeclarationGeneric *generic =
                        &metadata->generics[
                            type->array_length_generic_local - 1u];
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, generic->declared_type);
                } else {
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count, type->array_length_type);
                }
            } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
                cm_decl_reach_type(metadata, type_seen, type_queue,
                    &type_count, type->projection_self_type);
                cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                    &trait_count, type->projection_trait_local);
                cm_decl_reach_trait(metadata, trait_seen, trait_queue,
                    &trait_count, metadata->associated_items[
                        type->projection_associated_local - 1u]
                        .parent_local);
                for (child = 0u;
                        child < type->projection_argument_count; ++child)
                    cm_decl_reach_type(metadata, type_seen, type_queue,
                        &type_count,
                        type->projection_argument_types[child]);
            }
        }
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        if (!item_seen[index]) {
            cm_free(type_queue);
            cm_free(trait_queue);
            cm_free(item_queue);
            cm_free(type_seen);
            cm_free(trait_seen);
            cm_free(item_seen);
            return 0;
        }
    }
    for (index = 0u; index < metadata->trait_count; ++index) {
        if (!trait_seen[index]) {
            cm_free(type_queue);
            cm_free(trait_queue);
            cm_free(item_queue);
            cm_free(type_seen);
            cm_free(trait_seen);
            cm_free(item_seen);
            return 0;
        }
    }
    cm_free(type_queue);
    cm_free(trait_queue);
    cm_free(item_queue);
    cm_free(type_seen);
    cm_free(trait_seen);
    cm_free(item_seen);
    return 1;
}

static int cm_decl_validate_namespace(
    const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    if (metadata->namespace_count != 0u
        && metadata->namespace_entries == NULL) return 0;
    for (index = 0u; index < metadata->namespace_count; ++index) {
        const CmHirDeclarationNamespaceEntry *entry;
        entry = &metadata->namespace_entries[index];
        if (entry->owner_module == 0u
            || (size_t)entry->owner_module > metadata->module_count
            || !cm_decl_identifier(entry->name)) return 0;
        if (entry->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE) {
            if ((entry->target_kind != CM_HIR_DECL_TARGET_MODULE
                    && entry->target_kind != CM_HIR_DECL_TARGET_ITEM
                    && entry->target_kind != CM_HIR_DECL_TARGET_NOMINAL
                    && entry->target_kind
                        != CM_HIR_DECL_TARGET_ENUM_VARIANT
                    && entry->target_kind != CM_HIR_DECL_TARGET_PRIMITIVE)
                || entry->target_local == 0u
                || (entry->target_kind == CM_HIR_DECL_TARGET_MODULE
                    && (size_t)entry->target_local > metadata->module_count)
                || (entry->target_kind == CM_HIR_DECL_TARGET_ITEM
                    && (size_t)entry->target_local > metadata->item_count)
                || (entry->target_kind == CM_HIR_DECL_TARGET_NOMINAL
                    && (size_t)entry->target_local > metadata->trait_count)
                || (entry->target_kind
                        == CM_HIR_DECL_TARGET_ENUM_VARIANT
                    && cm_decl_variant_local(metadata,
                        entry->target_local, NULL) == NULL)
                || (entry->target_kind == CM_HIR_DECL_TARGET_PRIMITIVE
                    && !cm_decl_namespace_primitive(entry->target_local)))
                return 0;
            if (entry->target_kind == CM_HIR_DECL_TARGET_ITEM
                && metadata->items[entry->target_local - 1u]
                    .visibility.kind != CM_HIR_DECL_VISIBILITY_PUBLIC)
                return 0;
            if (entry->target_kind == CM_HIR_DECL_TARGET_NOMINAL
                && metadata->traits[entry->target_local - 1u]
                    .visibility.kind != CM_HIR_DECL_VISIBILITY_PUBLIC)
                return 0;
            if (entry->target_kind == CM_HIR_DECL_TARGET_ENUM_VARIANT) {
                const CmHirDeclarationItem *owner = NULL;
                (void)cm_decl_variant_local(metadata, entry->target_local,
                    &owner);
                if (owner == NULL || owner->visibility.kind
                        != CM_HIR_DECL_VISIBILITY_PUBLIC) return 0;
            }
            if (entry->target_kind == CM_HIR_DECL_TARGET_MODULE) {
                const CmHirDeclarationModule *target = &metadata->modules[
                    entry->target_local - 1u];
                if (entry->target_local == metadata->root_module
                    || target->parent_module != entry->owner_module
                    || !cm_decl_string_equal(entry->name, target->name))
                    return 0;
            }
        } else if (entry->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE) {
            if ((entry->target_kind != CM_HIR_DECL_TARGET_ITEM
                    && entry->target_kind != CM_HIR_DECL_TARGET_VALUE
                    && entry->target_kind
                        != CM_HIR_DECL_TARGET_ENUM_VARIANT)
                || entry->target_local == 0u
                || (entry->target_kind == CM_HIR_DECL_TARGET_ITEM
                    && ((size_t)entry->target_local > metadata->item_count
                        || metadata->items[entry->target_local - 1u].kind
                            != CM_HIR_DECL_ITEM_STRUCT
                        || metadata->items[entry->target_local - 1u]
                            .aggregate_form
                            != CM_HIR_DECL_AGGREGATE_UNIT))
                || (entry->target_kind == CM_HIR_DECL_TARGET_VALUE
                    && (size_t)entry->target_local > metadata->value_count)
                || (entry->target_kind
                        == CM_HIR_DECL_TARGET_ENUM_VARIANT
                    && cm_decl_variant_local(metadata,
                        entry->target_local, NULL) == NULL))
                return 0;
            if (entry->target_kind == CM_HIR_DECL_TARGET_ITEM
                && metadata->items[entry->target_local - 1u]
                    .visibility.kind != CM_HIR_DECL_VISIBILITY_PUBLIC)
                return 0;
            if (entry->target_kind == CM_HIR_DECL_TARGET_ENUM_VARIANT) {
                const CmHirDeclarationItem *owner = NULL;
                (void)cm_decl_variant_local(metadata, entry->target_local,
                    &owner);
                if (owner == NULL || owner->visibility.kind
                        != CM_HIR_DECL_VISIBILITY_PUBLIC) return 0;
            }
        } else {
            return 0;
        }
        if (index != 0u) {
            const CmHirDeclarationNamespaceEntry *prior;
            prior = &metadata->namespace_entries[index - 1u];
            if (cm_decl_namespace_compare(prior, entry) >= 0
                || (prior->owner_module == entry->owner_module
                    && prior->namespace_kind == entry->namespace_kind
                    && cm_decl_string_equal(prior->name, entry->name)))
                return 0;
        }
    }
    for (index = 0u; index < metadata->namespace_count; ++index) {
        const CmHirDeclarationNamespaceEntry *entry =
            &metadata->namespace_entries[index];
        CmHirDeclarationNamespaceEntry key;
        const CmHirDeclarationNamespaceEntry *mate;
        if (entry->target_kind != CM_HIR_DECL_TARGET_ENUM_VARIANT)
            continue;
        key = *entry;
        key.namespace_kind = entry->namespace_kind
                == CM_HIR_DECL_NAMESPACE_TYPE
            ? CM_HIR_DECL_NAMESPACE_VALUE
            : CM_HIR_DECL_NAMESPACE_TYPE;
        mate = cm_decl_namespace_find(metadata, &key);
        if (mate == NULL
            || mate->target_kind != CM_HIR_DECL_TARGET_ENUM_VARIANT
            || mate->target_local != entry->target_local) return 0;
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item;
        size_t entry_index;
        size_t type_defining_count;
        size_t constructor_defining_count;
        size_t target_count;
        item = &metadata->items[index];
        type_defining_count = 0u;
        constructor_defining_count = 0u;
        target_count = 0u;
        for (entry_index = 0u; entry_index < metadata->namespace_count;
                ++entry_index) {
            const CmHirDeclarationNamespaceEntry *entry;
            entry = &metadata->namespace_entries[entry_index];
            if (entry->target_kind == CM_HIR_DECL_TARGET_ITEM
                && entry->target_local == (uint32_t)(index + 1u))
                target_count += 1u;
            if (entry->owner_module == item->owner_module
                && entry->target_kind == CM_HIR_DECL_TARGET_ITEM
                && entry->target_local == (uint32_t)(index + 1u)
                && entry->export_ordinal == item->source_ordinal
                && cm_decl_string_equal(entry->name, item->name)) {
                if (entry->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE)
                    type_defining_count += 1u;
                else if (entry->namespace_kind
                        == CM_HIR_DECL_NAMESPACE_VALUE
                    && item->kind == CM_HIR_DECL_ITEM_STRUCT
                    && item->aggregate_form == CM_HIR_DECL_AGGREGATE_UNIT)
                    constructor_defining_count += 1u;
            }
        }
        if ((item->visibility.kind != CM_HIR_DECL_VISIBILITY_PUBLIC
                && target_count != 0u)
            || (item->visibility.kind == CM_HIR_DECL_VISIBILITY_PUBLIC
                && type_defining_count != 1u)
            || ((item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
                    || item->kind == CM_HIR_DECL_ITEM_ENUM
                    || item->kind == CM_HIR_DECL_ITEM_UNION
                    || (item->kind == CM_HIR_DECL_ITEM_STRUCT
                        && item->aggregate_form
                            != CM_HIR_DECL_AGGREGATE_UNIT))
                && constructor_defining_count != 0u)
            || (item->kind == CM_HIR_DECL_ITEM_STRUCT
                && constructor_defining_count > 1u)) return 0;
        if (item->visibility.kind != CM_HIR_DECL_VISIBILITY_PUBLIC) continue;
        for (entry_index = 0u; entry_index < metadata->namespace_count;
                ++entry_index) {
            const CmHirDeclarationNamespaceEntry *entry =
                &metadata->namespace_entries[entry_index];
            size_t counterpart_index;
            size_t counterpart_count;
            if (entry->target_kind != CM_HIR_DECL_TARGET_ITEM
                || entry->target_local != (uint32_t)(index + 1u)) continue;
            counterpart_count = 0u;
            for (counterpart_index = 0u;
                    counterpart_index < metadata->namespace_count;
                    ++counterpart_index) {
                const CmHirDeclarationNamespaceEntry *counterpart =
                    &metadata->namespace_entries[counterpart_index];
                if (counterpart->owner_module == entry->owner_module
                    && counterpart->namespace_kind
                        != entry->namespace_kind
                    && counterpart->target_kind == CM_HIR_DECL_TARGET_ITEM
                    && counterpart->target_local == entry->target_local
                    && counterpart->export_ordinal == entry->export_ordinal
                    && cm_decl_string_equal(counterpart->name,
                        entry->name)) counterpart_count += 1u;
            }
            if (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
                || item->kind == CM_HIR_DECL_ITEM_ENUM
                || item->kind == CM_HIR_DECL_ITEM_UNION
                || (item->kind == CM_HIR_DECL_ITEM_STRUCT
                    && item->aggregate_form
                        != CM_HIR_DECL_AGGREGATE_UNIT)) {
                if (entry->namespace_kind != CM_HIR_DECL_NAMESPACE_TYPE
                    || counterpart_count != 0u) return 0;
            } else if (entry->namespace_kind
                    == CM_HIR_DECL_NAMESPACE_VALUE) {
                if (counterpart_count != 1u) return 0;
            } else if (counterpart_count
                    != (constructor_defining_count == 0u ? 0u : 1u)) {
                return 0;
            }
        }
    }
    /* MODS is the complete structural module census.  A child module has an
     * NSPC entry exactly when it is publicly reachable; private modules are
     * retained as owners/ancestors without fabricating a public binding. */
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirDeclarationTrait *trait_value;
        size_t entry_index;
        size_t defining_count;
        size_t target_count;
        trait_value = &metadata->traits[index];
        defining_count = 0u;
        target_count = 0u;
        for (entry_index = 0u; entry_index < metadata->namespace_count;
                ++entry_index) {
            const CmHirDeclarationNamespaceEntry *entry;
            entry = &metadata->namespace_entries[entry_index];
            if (entry->target_kind == CM_HIR_DECL_TARGET_NOMINAL
                && entry->target_local == (uint32_t)(index + 1u))
                target_count += 1u;
            if (entry->owner_module == trait_value->owner_module
                && entry->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
                && entry->target_kind == CM_HIR_DECL_TARGET_NOMINAL
                && entry->target_local == (uint32_t)(index + 1u)
                && entry->export_ordinal == trait_value->source_ordinal
                && cm_decl_string_equal(entry->name, trait_value->name))
                defining_count += 1u;
        }
        if ((trait_value->visibility.kind == CM_HIR_DECL_VISIBILITY_PUBLIC
                && defining_count != 1u)
            || (trait_value->visibility.kind
                    != CM_HIR_DECL_VISIBILITY_PUBLIC
                && target_count != 0u)) return 0;
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirDeclarationValue *value;
        size_t entry_index;
        size_t defining_count;
        value = &metadata->values[index];
        defining_count = 0u;
        for (entry_index = 0u; entry_index < metadata->namespace_count;
                ++entry_index) {
            const CmHirDeclarationNamespaceEntry *entry;
            entry = &metadata->namespace_entries[entry_index];
            if (entry->owner_module == value->owner_module
                && entry->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
                && entry->target_kind == CM_HIR_DECL_TARGET_VALUE
                && entry->target_local == (uint32_t)(index + 1u)
                && entry->export_ordinal == value->source_ordinal
                && cm_decl_string_equal(entry->name, value->name)) {
                defining_count += 1u;
            }
        }
        if (defining_count != 1u) return 0;
    }
    return cm_decl_validate_private_item_reachability(metadata);
}

CmHirDeclarationMetadataStatus cm_hir_declaration_metadata_validate(
    const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    if (metadata == NULL) return CM_HIR_DECL_METADATA_INVALID_ARGUMENT;
    if (!cm_decl_count_valid(metadata->cfg_count,
            CM_HIR_DECL_METADATA_MAX_CFGS)
        || !cm_decl_count_valid(metadata->module_count,
            CM_HIR_DECL_METADATA_MAX_MODULES)
        || !cm_decl_count_valid(metadata->trait_count,
            CM_HIR_DECL_METADATA_MAX_NOMINALS)
        || !cm_decl_count_valid(metadata->associated_count,
            CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS)
        || !cm_decl_count_valid(metadata->generic_count,
            CM_HIR_DECL_METADATA_MAX_GENERICS)
        || !cm_decl_count_valid(metadata->type_count,
            CM_HIR_DECL_METADATA_MAX_TYPES)
        || !cm_decl_count_valid(metadata->item_count,
            CM_HIR_DECL_METADATA_MAX_ITEMS)
        || !cm_decl_count_valid(metadata->value_count,
            CM_HIR_DECL_METADATA_MAX_VALUES)
        || !cm_decl_count_valid(metadata->predicate_count,
            CM_HIR_DECL_METADATA_MAX_PREDICATES)
        || !cm_decl_count_valid(metadata->outlives_predicate_count,
            CM_HIR_DECL_METADATA_MAX_PREDICATES)
        || metadata->predicate_count
            > CM_HIR_DECL_METADATA_MAX_PREDICATES
                - metadata->outlives_predicate_count
        || !cm_decl_count_valid(metadata->namespace_count,
            CM_HIR_DECL_METADATA_MAX_NAMESPACE_ENTRIES))
        return CM_HIR_DECL_METADATA_LIMIT_EXCEEDED;
    if (!cm_decl_identifier(metadata->crate_name)
        || !cm_decl_bytes_valid(metadata->crate_disambiguator, 1u,
            CM_DECL_STRING_MAX)
        || metadata->edition < CM_HIR_DECL_EDITION_2015
        || metadata->edition > CM_HIR_DECL_EDITION_2024
        || !cm_decl_bytes_valid(metadata->target_triple, 1u,
            CM_DECL_STRING_MAX)
        || !cm_decl_bytes_valid(metadata->data_layout, 1u,
            CM_DECL_STRING_MAX)
        || (metadata->panic_strategy != CM_HIR_DECL_PANIC_ABORT
            && metadata->panic_strategy != CM_HIR_DECL_PANIC_UNWIND)
        || (metadata->cfg_count != 0u && metadata->cfgs == NULL))
        return CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR;
    for (index = 0u; index < metadata->cfg_count; ++index) {
        if (!cm_decl_bytes_valid(metadata->cfgs[index], 1u,
                CM_DECL_STRING_MAX)
            || (index != 0u && cm_decl_string_compare(
                metadata->cfgs[index - 1u], metadata->cfgs[index]) >= 0))
            return CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR;
    }
    if (!cm_decl_validate_modules(metadata)
        || !cm_decl_validate_traits(metadata)
        || !cm_decl_validate_items(metadata)
        || !cm_decl_validate_lang_items(metadata)
        || !cm_decl_validate_diagnostic_items(metadata)
        || !cm_decl_validate_values(metadata)
        || !cm_decl_validate_generics(metadata)
        || !cm_decl_validate_types(metadata)
        || !cm_decl_validate_aggregate_types(metadata)
        || !cm_decl_validate_associated_items(metadata)
        || !cm_decl_validate_predicates(metadata)
        || !cm_decl_validate_outlives_predicates(metadata)
        || !cm_decl_validate_value_type_scopes(metadata)
        || !cm_decl_validate_self_type_scopes(metadata)
        || !cm_decl_validate_erased_type_roots(metadata)
        || !cm_decl_validate_callable_profile(metadata)
        || !cm_decl_validate_clone_profile(metadata)
        || !cm_decl_validate_from_fn_profile(metadata)
        || !cm_decl_validate_type_reachability(metadata)
        || !cm_decl_validate_namespace(metadata))
        return CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR;
    return CM_HIR_DECL_METADATA_OK;
}

static void cm_decl_writer(CmHirMetadataWriter *writer, CmByteBuf *buffer)
{
    cm_byte_buf_init(buffer);
    cm_hir_metadata_writer_init(writer, buffer,
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
}

static CmHirMetadataStatus cm_decl_write_string(CmHirMetadataWriter *writer,
    CmHirDeclarationString value)
{
    CmHirMetadataStatus status;
    if (value.length > (size_t)UINT32_MAX)
        return CM_HIR_METADATA_LIMIT_EXCEEDED;
    status = cm_hir_metadata_write_u32(writer, (uint32_t)value.length);
    if (status == CM_HIR_METADATA_OK)
        status = cm_hir_metadata_write_bytes(writer, value.data,
            value.length);
    return status;
}

static CmHirDeclarationMetadataStatus cm_decl_writer_status(
    const CmHirMetadataWriter *writer)
{
    CmHirMetadataStatus status;
    status = cm_hir_metadata_writer_status(writer);
    if (status == CM_HIR_METADATA_OK) return CM_HIR_DECL_METADATA_OK;
    if (status == CM_HIR_METADATA_LIMIT_EXCEEDED
        || status == CM_HIR_METADATA_LENGTH_OVERFLOW)
        return CM_HIR_DECL_METADATA_LIMIT_EXCEEDED;
    return CM_HIR_DECL_METADATA_INVALID_FORMAT;
}

static void cm_decl_write_visibility(CmHirMetadataWriter *writer)
{
    cm_hir_metadata_write_u8(writer, UINT8_C(2));
    cm_hir_metadata_write_u8(writer, UINT8_C(0));
    cm_hir_metadata_write_u16(writer, UINT16_C(0));
    cm_hir_metadata_write_u32(writer, UINT32_C(0));
}

static void cm_decl_write_item_visibility(CmHirMetadataWriter *writer,
    const CmHirDeclarationVisibility *visibility)
{
    cm_hir_metadata_write_u8(writer, visibility->kind);
    cm_hir_metadata_write_u8(writer, UINT8_C(0));
    cm_hir_metadata_write_u16(writer, UINT16_C(0));
    cm_hir_metadata_write_u32(writer, visibility->restriction_module);
}

static CmHirDeclarationMetadataStatus cm_decl_build_sections(
    const CmHirDeclarationMetadata *metadata,
    CmByteBuf sections[CM_DECL_SECTION_COUNT])
{
    CmHirMetadataWriter writer;
    size_t index;
    for (index = 0u; index < CM_DECL_SECTION_COUNT; ++index)
        cm_byte_buf_init(&sections[index]);

    cm_hir_metadata_writer_init(&writer, &sections[0],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_decl_write_string(&writer, metadata->crate_name);
    cm_hir_metadata_write_u8(&writer, metadata->edition);
    cm_hir_metadata_write_u32(&writer, metadata->root_module);
    if (cm_decl_writer_status(&writer) != CM_HIR_DECL_METADATA_OK)
        return cm_decl_writer_status(&writer);

    cm_hir_metadata_writer_init(&writer, &sections[2],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->module_count);
    for (index = 0u; index < metadata->module_count; ++index) {
        cm_hir_metadata_write_u32(&writer,
            metadata->modules[index].parent_module);
        cm_decl_write_string(&writer, metadata->modules[index].name);
    }

    cm_hir_metadata_writer_init(&writer, &sections[3],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->trait_count);
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirDeclarationTrait *trait_value;
        uint32_t child;
        trait_value = &metadata->traits[index];
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(2));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, trait_value->owner_module);
        cm_decl_write_string(&writer, trait_value->name);
        cm_decl_write_item_visibility(&writer, &trait_value->visibility);
        cm_hir_metadata_write_u32(&writer, trait_value->source_ordinal);
        cm_hir_metadata_write_u32(&writer, trait_value->generic_start);
        cm_hir_metadata_write_u32(&writer, trait_value->generic_count);
        cm_hir_metadata_write_u32(&writer,
            trait_value->predicate_scope_start);
        cm_hir_metadata_write_u32(&writer,
            trait_value->predicate_scope_count);
        cm_hir_metadata_write_u32(&writer, trait_value->predicate_start);
        cm_hir_metadata_write_u32(&writer, trait_value->predicate_count);
        cm_hir_metadata_write_u32(&writer, trait_value->outlives_start);
        cm_hir_metadata_write_u32(&writer, trait_value->outlives_count);
        cm_hir_metadata_write_u32(&writer, trait_value->associated_start);
        cm_hir_metadata_write_u32(&writer, trait_value->associated_count);
        cm_hir_metadata_write_u8(&writer, trait_value->safety);
        cm_hir_metadata_write_u8(&writer, trait_value->flags);
        cm_hir_metadata_write_u16(&writer, trait_value->compiler_flags);
        cm_hir_metadata_write_u32(&writer,
            trait_value->supertrait_count);
        for (child = 0u; child < trait_value->supertrait_count; ++child) {
            const CmHirDeclarationSupertrait *supertrait =
                &trait_value->supertraits[child];
            uint32_t argument;
            cm_hir_metadata_write_u8(&writer, supertrait->modifier);
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
            cm_hir_metadata_write_u32(&writer, supertrait->trait_local);
            cm_hir_metadata_write_u32(&writer,
                supertrait->argument_count);
            for (argument = 0u; argument < supertrait->argument_count;
                    ++argument)
                cm_hir_metadata_write_u32(&writer,
                    supertrait->argument_types[argument]);
            cm_hir_metadata_write_u32(&writer, UINT32_C(0));
            cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        }
        if ((trait_value->flags
                & CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM) != 0u)
            cm_decl_write_string(&writer, trait_value->diagnostic_item);
        if ((trait_value->flags
                & CM_HIR_DECL_TRAIT_HAS_LANG_ITEM) != 0u)
            cm_decl_write_string(&writer, trait_value->lang_item);
    }

    cm_hir_metadata_writer_init(&writer, &sections[4],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer,
        (uint32_t)metadata->associated_count);
    for (index = 0u; index < metadata->associated_count; ++index) {
        const CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        uint32_t child;
        cm_hir_metadata_write_u8(&writer, associated->kind);
        cm_hir_metadata_write_u8(&writer, associated->parent_kind);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, associated->parent_local);
        cm_hir_metadata_write_u32(&writer,
            associated->implemented_associated_local);
        cm_decl_write_string(&writer, associated->name);
        cm_hir_metadata_write_u8(&writer, associated->visibility.kind);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer,
            associated->visibility.restriction_module);
        cm_hir_metadata_write_u32(&writer, associated->source_ordinal);
        cm_hir_metadata_write_u8(&writer, associated->is_specializable);
        cm_hir_metadata_write_u8(&writer, associated->flags);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, associated->generic_start);
        cm_hir_metadata_write_u32(&writer, associated->generic_count);
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, associated->predicate_start);
        cm_hir_metadata_write_u32(&writer, associated->predicate_count);
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_METHOD) {
            cm_hir_metadata_write_u8(&writer, associated->receiver);
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
            cm_hir_metadata_write_u32(&writer,
                associated->parameter_count);
            for (child = 0u; child < associated->parameter_count; ++child)
                cm_hir_metadata_write_u32(&writer,
                    associated->parameter_types[child]);
            cm_hir_metadata_write_u32(&writer, associated->return_type);
            cm_decl_write_string(&writer, associated->abi);
            cm_hir_metadata_write_u8(&writer, associated->safety);
            cm_hir_metadata_write_u8(&writer, associated->is_const);
            cm_hir_metadata_write_u8(&writer, associated->is_async);
            cm_hir_metadata_write_u8(&writer, associated->is_variadic);
            cm_hir_metadata_write_u8(&writer,
                associated->has_default_body);
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        }
        if ((associated->flags
                & CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM) != 0u)
            cm_decl_write_string(&writer, associated->lang_item);
    }

    cm_hir_metadata_writer_init(&writer, &sections[5],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->generic_count);
    for (index = 0u; index < metadata->generic_count; ++index) {
        const CmHirDeclarationGeneric *generic;
        generic = &metadata->generics[index];
        cm_hir_metadata_write_u8(&writer, generic->owner_kind);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, generic->owner_local);
        cm_hir_metadata_write_u32(&writer, generic->index);
        cm_hir_metadata_write_u8(&writer, generic->kind);
        cm_hir_metadata_write_u8(&writer, generic->is_relaxed_sized);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_decl_write_string(&writer, generic->name);
        cm_hir_metadata_write_u8(&writer, generic->has_default);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, generic->declared_type);
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    }

    cm_hir_metadata_writer_init(&writer, &sections[6],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->type_count);
    for (index = 0u; index < metadata->type_count; ++index) {
        const CmHirDeclarationType *type;
        type = &metadata->types[index];
        cm_hir_metadata_write_u8(&writer, type->kind);
        cm_hir_metadata_write_u8(&writer,
            type->kind == CM_HIR_DECL_TYPE_ARRAY
                ? type->array_length_kind : UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        if (type->kind == CM_HIR_DECL_TYPE_PRIMITIVE) {
            cm_hir_metadata_write_u8(&writer, type->primitive);
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        } else if (type->kind == CM_HIR_DECL_TYPE_GENERIC) {
            cm_hir_metadata_write_u32(&writer, type->generic_local);
        } else if (type->kind == CM_HIR_DECL_TYPE_NAMED_ADT) {
            cm_hir_metadata_write_u32(&writer, type->item_local);
        } else if (type->kind == CM_HIR_DECL_TYPE_SELF) {
            cm_hir_metadata_write_u32(&writer, type->self_trait_local);
        } else if (type->kind == CM_HIR_DECL_TYPE_SLICE) {
            cm_hir_metadata_write_u32(&writer, type->child_type);
        } else if (type->kind == CM_HIR_DECL_TYPE_RAW_POINTER) {
            cm_hir_metadata_write_u8(&writer, type->mutability);
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
            cm_hir_metadata_write_u32(&writer, type->child_type);
        } else if (type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            cm_hir_metadata_write_u8(&writer, type->region.kind);
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
            cm_hir_metadata_write_u32(&writer,
                type->region.generic_local);
            cm_hir_metadata_write_u32(&writer, type->region.binder_index);
            cm_hir_metadata_write_u8(&writer, type->mutability);
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
            cm_hir_metadata_write_u32(&writer, type->child_type);
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            uint32_t child;
            cm_hir_metadata_write_u32(&writer, type->item_local);
            cm_hir_metadata_write_u32(&writer, type->argument_count);
            for (child = 0u; child < type->argument_count; ++child)
                cm_hir_metadata_write_u32(&writer,
                    type->argument_types[child]);
        } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
            uint32_t child;
            cm_hir_metadata_write_u32(&writer, type->element_count);
            for (child = 0u; child < type->element_count; ++child)
                cm_hir_metadata_write_u32(&writer,
                    type->element_types[child]);
        } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY) {
            cm_hir_metadata_write_u32(&writer, type->child_type);
            if (type->array_length_kind
                    == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER) {
                cm_hir_metadata_write_u32(&writer,
                    type->array_length_generic_local);
                cm_hir_metadata_write_u64(&writer, UINT64_C(0));
                cm_hir_metadata_write_u64(&writer, UINT64_C(0));
            } else {
                cm_hir_metadata_write_u32(&writer,
                    type->array_length_type);
                cm_hir_metadata_write_u64(&writer,
                    type->array_length_low_bits);
                cm_hir_metadata_write_u64(&writer,
                    type->array_length_high_bits);
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
            uint32_t projection_child;
            cm_hir_metadata_write_u32(&writer,
                type->projection_self_type);
            cm_hir_metadata_write_u32(&writer,
                type->projection_trait_local);
            cm_hir_metadata_write_u32(&writer,
                type->projection_associated_local);
            cm_hir_metadata_write_u32(&writer,
                type->projection_argument_count);
            for (projection_child = 0u;
                    projection_child < type->projection_argument_count;
                    ++projection_child)
                cm_hir_metadata_write_u32(&writer,
                    type->projection_argument_types[projection_child]);
        }
    }

    cm_hir_metadata_writer_init(&writer, &sections[7],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->item_count);
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item;
        item = &metadata->items[index];
        cm_hir_metadata_write_u8(&writer, item->kind);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, item->owner_module);
        cm_decl_write_string(&writer, item->name);
        cm_decl_write_item_visibility(&writer, &item->visibility);
        cm_hir_metadata_write_u32(&writer, item->source_ordinal);
        cm_hir_metadata_write_u32(&writer, item->generic_start);
        cm_hir_metadata_write_u32(&writer, item->generic_count);
        cm_hir_metadata_write_u32(&writer, item->predicate_scope_start);
        cm_hir_metadata_write_u32(&writer, item->predicate_scope_count);
        cm_hir_metadata_write_u32(&writer, item->predicate_start);
        cm_hir_metadata_write_u32(&writer, item->predicate_count);
        cm_hir_metadata_write_u32(&writer, item->outlives_start);
        cm_hir_metadata_write_u32(&writer, item->outlives_count);
        if (item->kind == CM_HIR_DECL_ITEM_STRUCT
                || item->kind == CM_HIR_DECL_ITEM_UNION) {
            uint32_t child;
            cm_hir_metadata_write_u8(&writer, item->aggregate_form);
            cm_hir_metadata_write_u8(&writer, item->aggregate_repr);
            cm_hir_metadata_write_u16(&writer, item->aggregate_flags);
            cm_hir_metadata_write_u32(&writer, item->field_count);
            for (child = 0u; child < item->field_count; ++child) {
                const CmHirDeclarationField *field = &item->fields[child];
                cm_decl_write_string(&writer, field->name);
                cm_decl_write_item_visibility(&writer, &field->visibility);
                cm_hir_metadata_write_u32(&writer,
                    field->source_ordinal);
                cm_hir_metadata_write_u32(&writer, field->type_local);
            }
            if ((item->aggregate_flags
                    & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u)
                cm_decl_write_string(&writer, item->lang_item);
            if ((item->aggregate_flags
                    & CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM) != 0u)
                cm_decl_write_string(&writer, item->diagnostic_item);
        } else if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t child;
            cm_hir_metadata_write_u8(&writer,
                item->enum_repr_primitive);
            cm_hir_metadata_write_u8(&writer, item->enum_flags);
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
            cm_hir_metadata_write_u32(&writer, item->variant_count);
            for (child = 0u; child < item->variant_count; ++child) {
                const CmHirDeclarationVariant *variant =
                    &item->variants[child];
                uint32_t field_index;
                cm_hir_metadata_write_u8(&writer, variant->kind);
                cm_hir_metadata_write_u8(&writer,
                    variant->discriminant_primitive);
                cm_hir_metadata_write_u16(&writer, variant->flags);
                cm_decl_write_string(&writer, variant->name);
                cm_hir_metadata_write_u32(&writer,
                    variant->source_ordinal);
                cm_hir_metadata_write_u64(&writer,
                    variant->discriminant_low);
                cm_hir_metadata_write_u64(&writer,
                    variant->discriminant_high);
                if (variant->kind == CM_HIR_DECL_VARIANT_TUPLE) {
                    cm_hir_metadata_write_u32(&writer,
                        variant->field_count);
                    for (field_index = 0u;
                            field_index < variant->field_count;
                            ++field_index) {
                        cm_hir_metadata_write_u32(&writer,
                            variant->fields[field_index].source_ordinal);
                        cm_hir_metadata_write_u32(&writer,
                            variant->fields[field_index].type_local);
                    }
                }
                if ((variant->flags
                        & CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) != 0u)
                    cm_decl_write_string(&writer, variant->lang_item);
            }
            if (item->enum_repr_primitive == CM_HIR_DECL_ENUM_REPR_RUST)
                cm_decl_write_string(&writer, item->diagnostic_item);
            if ((item->enum_flags
                    & CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u)
                cm_decl_write_string(&writer, item->enum_lang_item);
        } else {
            cm_hir_metadata_write_u32(&writer, item->alias_target_type);
        }
    }

    cm_hir_metadata_writer_init(&writer, &sections[8],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->value_count);
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirDeclarationValue *value;
        uint32_t child;
        value = &metadata->values[index];
        cm_hir_metadata_write_u8(&writer, value->kind);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, value->owner_module);
        cm_decl_write_string(&writer, value->name);
        cm_decl_write_visibility(&writer);
        cm_hir_metadata_write_u32(&writer, value->source_ordinal);
        cm_hir_metadata_write_u32(&writer, value->generic_start);
        cm_hir_metadata_write_u32(&writer, value->generic_count);
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, value->predicate_start);
        cm_hir_metadata_write_u32(&writer, value->predicate_count);
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        if (value->kind == CM_HIR_DECL_VALUE_FUNCTION) {
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u32(&writer, value->parameter_count);
            for (child = 0u; child < value->parameter_count; ++child)
                cm_hir_metadata_write_u32(&writer,
                    value->parameter_types[child]);
            cm_hir_metadata_write_u32(&writer, value->return_type);
            cm_hir_metadata_write_u8(&writer, value->has_body);
            cm_hir_metadata_write_u8(&writer, value->is_const);
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        } else {
            cm_hir_metadata_write_u32(&writer, value->declared_type);
            cm_hir_metadata_write_u8(&writer, value->mutability);
            cm_hir_metadata_write_u8(&writer, value->has_body);
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        }
    }

    cm_hir_metadata_writer_init(&writer, &sections[9],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->predicate_count);
    cm_hir_metadata_write_u32(&writer,
        (uint32_t)metadata->outlives_predicate_count);
    for (index = 0u; index < metadata->predicate_count; ++index) {
        const CmHirDeclarationPredicate *predicate;
        uint32_t child;
        predicate = &metadata->predicates[index];
        cm_hir_metadata_write_u8(&writer, UINT8_C(4));
        cm_hir_metadata_write_u8(&writer, predicate->owner_kind);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer,
            predicate->owner_kind == CM_HIR_DECL_PREDICATE_OWNER_VALUE
                ? predicate->owner_value
                : predicate->owner_kind
                    == CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
                    ? predicate->owner_associated
                    : predicate->owner_kind
                        == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
                        ? predicate->owner_nominal : predicate->owner_item);
        cm_hir_metadata_write_u32(&writer, predicate->ordinal);
        cm_hir_metadata_write_u32(&writer, predicate->subject_type);
        cm_hir_metadata_write_u32(&writer, predicate->trait_local);
        cm_hir_metadata_write_u32(&writer, predicate->argument_count);
        for (child = 0u; child < predicate->argument_count; ++child)
            cm_hir_metadata_write_u32(&writer,
                predicate->argument_types[child]);
        cm_hir_metadata_write_u32(&writer, predicate->equality_count);
        for (child = 0u; child < predicate->equality_count; ++child) {
            cm_hir_metadata_write_u32(&writer,
                predicate->equalities[child].associated_local);
            cm_hir_metadata_write_u32(&writer,
                predicate->equalities[child].value_type);
        }
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u8(&writer,
            (uint8_t)(predicate->modifier + UINT8_C(1)));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
    }
    for (index = 0u; index < metadata->outlives_predicate_count; ++index) {
        const CmHirDeclarationOutlivesPredicate *predicate =
            &metadata->outlives_predicates[index];
        cm_hir_metadata_write_u8(&writer, UINT8_C(5));
        cm_hir_metadata_write_u8(&writer, predicate->owner_kind);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, predicate->owner_local);
        cm_hir_metadata_write_u32(&writer, predicate->ordinal);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, predicate->subject_type);
        cm_hir_metadata_write_u8(&writer, predicate->bound.kind);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, predicate->bound.generic_local);
        cm_hir_metadata_write_u32(&writer, predicate->bound.binder_index);
        cm_hir_metadata_write_u32(&writer, predicate->scope);
    }

    cm_hir_metadata_writer_init(&writer, &sections[10],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, UINT32_C(0));

    cm_hir_metadata_writer_init(&writer, &sections[11],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->namespace_count);
    for (index = 0u; index < metadata->namespace_count; ++index) {
        const CmHirDeclarationNamespaceEntry *entry;
        entry = &metadata->namespace_entries[index];
        cm_hir_metadata_write_u32(&writer, entry->owner_module);
        cm_hir_metadata_write_u8(&writer, entry->namespace_kind);
        cm_hir_metadata_write_u8(&writer, entry->target_kind);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_decl_write_string(&writer, entry->name);
        cm_hir_metadata_write_u32(&writer, entry->target_local);
        cm_decl_write_visibility(&writer);
        cm_hir_metadata_write_u32(&writer, entry->export_ordinal);
    }
    for (index = 0u; index < CM_DECL_SECTION_COUNT; ++index) {
        if (index != 1u && sections[index].len
                > (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE)
            return CM_HIR_DECL_METADATA_LIMIT_EXCEEDED;
    }
    return cm_decl_writer_status(&writer);
}

static uint32_t cm_decl_empty_crc(void)
{
    static const unsigned char empty[4] = { 0u, 0u, 0u, 0u };
    return cm_hir_metadata_crc32(empty, sizeof(empty));
}

static CmHirDeclarationMetadataStatus cm_decl_build_manifest(
    const CmHirDeclarationMetadata *metadata,
    CmByteBuf sections[CM_DECL_SECTION_COUNT], CmDeclFamily families[14])
{
    CmHirMetadataWriter writer;
    CmByteBuf module_family;
    size_t index;
    static const uint8_t complete[CM_DECL_FAMILY_COUNT] = {
        1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u,
        0u, 0u, 0u, 0u, 0u, 0u
    };
    cm_decl_writer(&writer, &module_family);
    cm_hir_metadata_write_bytes(&writer, sections[2].data, sections[2].len);
    cm_hir_metadata_write_bytes(&writer, sections[11].data,
        sections[11].len);
    if (cm_decl_writer_status(&writer) != CM_HIR_DECL_METADATA_OK) {
        cm_byte_buf_destroy(&module_family);
        return cm_decl_writer_status(&writer);
    }
    for (index = 0u; index < CM_DECL_FAMILY_COUNT; ++index) {
        families[index].state = complete[index] ? UINT8_C(2) : UINT8_C(0);
        families[index].count = UINT32_C(0);
        families[index].crc = cm_decl_empty_crc();
    }
    families[0].count = (uint32_t)(metadata->module_count
        + metadata->namespace_count);
    families[0].crc = cm_hir_metadata_crc32(module_family.data,
        module_family.len);
    families[1].count = (uint32_t)metadata->item_count;
    families[1].crc = cm_hir_metadata_crc32(sections[7].data,
        sections[7].len);
    families[2].count = (uint32_t)metadata->value_count;
    families[2].crc = cm_hir_metadata_crc32(sections[8].data,
        sections[8].len);
    families[3].count = (uint32_t)metadata->trait_count;
    families[3].crc = cm_hir_metadata_crc32(sections[3].data,
        sections[3].len);
    families[5].count = (uint32_t)metadata->associated_count;
    families[5].crc = cm_hir_metadata_crc32(sections[4].data,
        sections[4].len);
    families[8].crc = cm_hir_metadata_crc32(sections[10].data,
        sections[10].len);
    families[9].crc = families[8].crc;

    cm_hir_metadata_writer_init(&writer, &sections[1],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u8(&writer, UINT8_C(1));
    cm_hir_metadata_write_u8(&writer, UINT8_C(1));
    cm_hir_metadata_write_u8(&writer, UINT8_C(0));
    cm_hir_metadata_write_u8(&writer, UINT8_C(0));
    cm_decl_write_string(&writer, metadata->crate_name);
    cm_hir_metadata_write_u32(&writer,
        (uint32_t)metadata->crate_disambiguator.length);
    cm_hir_metadata_write_bytes(&writer, metadata->crate_disambiguator.data,
        metadata->crate_disambiguator.length);
    cm_hir_metadata_write_u8(&writer, metadata->edition);
    cm_decl_write_string(&writer, metadata->target_triple);
    cm_decl_write_string(&writer, metadata->data_layout);
    cm_hir_metadata_write_u8(&writer, metadata->panic_strategy);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->cfg_count);
    for (index = 0u; index < metadata->cfg_count; ++index)
        cm_decl_write_string(&writer, metadata->cfgs[index]);
    cm_hir_metadata_write_u32(&writer, UINT32_C(14));
    for (index = 0u; index < CM_DECL_FAMILY_COUNT; ++index) {
        cm_hir_metadata_write_u8(&writer, (uint8_t)(index + 1u));
        cm_hir_metadata_write_u8(&writer, families[index].state);
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, families[index].count);
        cm_hir_metadata_write_u32(&writer, families[index].crc);
    }
    cm_byte_buf_destroy(&module_family);
    return cm_decl_writer_status(&writer);
}

CmHirDeclarationMetadataStatus cm_hir_declaration_metadata_encode(
    const CmHirDeclarationMetadata *metadata, CmByteBuf *output)
{
    CmByteBuf sections[CM_DECL_SECTION_COUNT];
    CmByteBuf payload;
    CmByteBuf encoded;
    CmHirMetadataWriter writer;
    CmDeclFamily families[CM_DECL_FAMILY_COUNT];
    CmHirDeclarationMetadataStatus status;
    CmHirMetadataStatus wire_status;
    size_t index;
    if (output == NULL) return CM_HIR_DECL_METADATA_INVALID_ARGUMENT;
    status = cm_hir_declaration_metadata_validate(metadata);
    if (status != CM_HIR_DECL_METADATA_OK) return status;
    status = cm_decl_build_sections(metadata, sections);
    if (status != CM_HIR_DECL_METADATA_OK) goto done;
    status = cm_decl_build_manifest(metadata, sections, families);
    if (status != CM_HIR_DECL_METADATA_OK) goto done;
    cm_decl_writer(&writer, &payload);
    for (index = 0u; index < CM_DECL_SECTION_COUNT; ++index)
        cm_hir_metadata_write_section(&writer, cm_decl_tags[index],
            sections[index].data, sections[index].len);
    status = cm_decl_writer_status(&writer);
    if (status != CM_HIR_DECL_METADATA_OK) {
        cm_byte_buf_destroy(&payload);
        goto done;
    }
    cm_byte_buf_init(&encoded);
    wire_status = cm_hir_metadata_encode_envelope_version(&encoded,
        CM_HIR_DECL_METADATA_MAJOR, CM_HIR_DECL_METADATA_MINOR,
        UINT32_C(0), payload.data, payload.len);
    cm_byte_buf_destroy(&payload);
    if (wire_status != CM_HIR_METADATA_OK) {
        cm_byte_buf_destroy(&encoded);
        status = wire_status == CM_HIR_METADATA_LIMIT_EXCEEDED
            || wire_status == CM_HIR_METADATA_LENGTH_OVERFLOW
            ? CM_HIR_DECL_METADATA_LIMIT_EXCEEDED
            : CM_HIR_DECL_METADATA_INVALID_FORMAT;
        goto done;
    }
    cm_byte_buf_destroy(output);
    *output = encoded;
    status = CM_HIR_DECL_METADATA_OK;
done:
    for (index = 0u; index < CM_DECL_SECTION_COUNT; ++index)
        cm_byte_buf_destroy(&sections[index]);
    return status;
}

static int cm_decl_read_u8(CmHirMetadataReader *reader, uint8_t expected)
{
    uint8_t value;
    return cm_hir_metadata_read_u8(reader, &value) == CM_HIR_METADATA_OK
        && value == expected;
}

static int cm_decl_read_u16(CmHirMetadataReader *reader, uint16_t expected)
{
    uint16_t value;
    return cm_hir_metadata_read_u16(reader, &value) == CM_HIR_METADATA_OK
        && value == expected;
}

static int cm_decl_read_u32(CmHirMetadataReader *reader, uint32_t expected)
{
    uint32_t value;
    return cm_hir_metadata_read_u32(reader, &value) == CM_HIR_METADATA_OK
        && value == expected;
}

static int cm_decl_read_u64(CmHirMetadataReader *reader, uint64_t expected)
{
    uint64_t value;
    return cm_hir_metadata_read_u64(reader, &value) == CM_HIR_METADATA_OK
        && value == expected;
}

static int cm_decl_read_string(CmHirMetadataReader *reader,
    CmHirDeclarationString *out)
{
    uint32_t length;
    const unsigned char *bytes;
    unsigned char *copy;
    if (cm_hir_metadata_read_u32(reader, &length) != CM_HIR_METADATA_OK
        || length > (uint32_t)CM_DECL_STRING_MAX
        || cm_hir_metadata_read_bytes(reader, (size_t)length, &bytes)
            != CM_HIR_METADATA_OK) return 0;
    copy = (unsigned char *)cm_alloc((size_t)length + 1u);
    if (length != 0u) memcpy(copy, bytes, (size_t)length);
    copy[length] = 0u;
    out->data = copy;
    out->length = (size_t)length;
    return 1;
}

static int cm_decl_read_string_equal(CmHirMetadataReader *reader,
    CmHirDeclarationString expected)
{
    uint32_t length;
    const unsigned char *bytes;
    return cm_hir_metadata_read_u32(reader, &length) == CM_HIR_METADATA_OK
        && (size_t)length == expected.length
        && cm_hir_metadata_read_bytes(reader, (size_t)length, &bytes)
            == CM_HIR_METADATA_OK
        && (length == 0u || memcmp(bytes, expected.data,
            (size_t)length) == 0);
}

static int cm_decl_read_count(CmHirMetadataReader *reader, size_t maximum,
    size_t *out)
{
    uint32_t count;
    if (cm_hir_metadata_read_u32(reader, &count) != CM_HIR_METADATA_OK
        || !cm_decl_count_valid((size_t)count, maximum)) return 0;
    *out = (size_t)count;
    return 1;
}

static int cm_decl_reader_done(CmHirMetadataReader *reader)
{
    return cm_hir_metadata_reader_finish(reader) == CM_HIR_METADATA_OK;
}

static int cm_decl_parse_manifest(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata, CmDeclFamily families[14])
{
    CmHirMetadataReader reader;
    uint32_t disambiguator_length;
    const unsigned char *disambiguator;
    size_t index;
    uint32_t family_count;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_u8(&reader, UINT8_C(1))
        || !cm_decl_read_u8(&reader, UINT8_C(1))
        || !cm_decl_read_u8(&reader, UINT8_C(0))
        || !cm_decl_read_u8(&reader, UINT8_C(0))
        || !cm_decl_read_string(&reader, &metadata->crate_name)
        || cm_hir_metadata_read_u32(&reader, &disambiguator_length)
            != CM_HIR_METADATA_OK
        || disambiguator_length == 0u
        || disambiguator_length > (uint32_t)CM_DECL_STRING_MAX
        || cm_hir_metadata_read_bytes(&reader,
            (size_t)disambiguator_length, &disambiguator)
            != CM_HIR_METADATA_OK) return 0;
    metadata->crate_disambiguator.data = (unsigned char *)cm_alloc(
        (size_t)disambiguator_length + 1u);
    memcpy(metadata->crate_disambiguator.data, disambiguator,
        (size_t)disambiguator_length);
    metadata->crate_disambiguator.data[disambiguator_length] = 0u;
    metadata->crate_disambiguator.length = (size_t)disambiguator_length;
    if (cm_hir_metadata_read_u8(&reader, &metadata->edition)
            != CM_HIR_METADATA_OK
        || !cm_decl_read_string(&reader, &metadata->target_triple)
        || !cm_decl_read_string(&reader, &metadata->data_layout)
        || cm_hir_metadata_read_u8(&reader, &metadata->panic_strategy)
            != CM_HIR_METADATA_OK
        || !cm_decl_read_count(&reader, CM_HIR_DECL_METADATA_MAX_CFGS,
            &metadata->cfg_count)) return 0;
    metadata->cfgs = metadata->cfg_count == 0u ? NULL
        : (CmHirDeclarationString *)cm_alloc_zeroed(metadata->cfg_count,
            sizeof(CmHirDeclarationString));
    for (index = 0u; index < metadata->cfg_count; ++index) {
        if (!cm_decl_read_string(&reader, &metadata->cfgs[index])) return 0;
    }
    if (cm_hir_metadata_read_u32(&reader, &family_count)
            != CM_HIR_METADATA_OK
        || family_count != UINT32_C(14)) return 0;
    for (index = 0u; index < CM_DECL_FAMILY_COUNT; ++index) {
        uint8_t tag;
        if (cm_hir_metadata_read_u8(&reader, &tag) != CM_HIR_METADATA_OK
            || tag != (uint8_t)(index + 1u)
            || cm_hir_metadata_read_u8(&reader, &families[index].state)
                != CM_HIR_METADATA_OK
            || (families[index].state != UINT8_C(0)
                && families[index].state != UINT8_C(1)
                && families[index].state != UINT8_C(2))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &families[index].count)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &families[index].crc)
                != CM_HIR_METADATA_OK) return 0;
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_parse_crat(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    uint8_t edition;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    return cm_decl_read_string_equal(&reader, metadata->crate_name)
        && cm_hir_metadata_read_u8(&reader, &edition) == CM_HIR_METADATA_OK
        && edition == metadata->edition
        && cm_hir_metadata_read_u32(&reader, &metadata->root_module)
            == CM_HIR_METADATA_OK
        && cm_decl_reader_done(&reader);
}

static int cm_decl_parse_modules(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader, CM_HIR_DECL_METADATA_MAX_MODULES,
            &metadata->module_count)
        || metadata->module_count == 0u) return 0;
    metadata->modules = (CmHirDeclarationModule *)cm_alloc_zeroed(
        metadata->module_count, sizeof(CmHirDeclarationModule));
    for (index = 0u; index < metadata->module_count; ++index) {
        if (cm_hir_metadata_read_u32(&reader,
                &metadata->modules[index].parent_module)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_string(&reader,
                &metadata->modules[index].name)) return 0;
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_read_visibility(CmHirMetadataReader *reader)
{
    return cm_decl_read_u8(reader, UINT8_C(2))
        && cm_decl_read_u8(reader, UINT8_C(0))
        && cm_decl_read_u16(reader, UINT16_C(0))
        && cm_decl_read_u32(reader, UINT32_C(0));
}

static int cm_decl_read_item_visibility(CmHirMetadataReader *reader,
    CmHirDeclarationVisibility *visibility)
{
    return cm_hir_metadata_read_u8(reader, &visibility->kind)
            == CM_HIR_METADATA_OK
        && cm_decl_read_u8(reader, UINT8_C(0))
        && cm_decl_read_u16(reader, UINT16_C(0))
        && cm_hir_metadata_read_u32(reader,
            &visibility->restriction_module) == CM_HIR_METADATA_OK;
}

static int cm_decl_parse_traits(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    size_t total_arguments;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader, CM_HIR_DECL_METADATA_MAX_NOMINALS,
            &metadata->trait_count)) return 0;
    metadata->traits = metadata->trait_count == 0u ? NULL
        : (CmHirDeclarationTrait *)cm_alloc_zeroed(metadata->trait_count,
            sizeof(CmHirDeclarationTrait));
    total_arguments = 0u;
    for (index = 0u; index < metadata->trait_count; ++index) {
        CmHirDeclarationTrait *trait_value;
        uint32_t child;
        trait_value = &metadata->traits[index];
        if (!cm_decl_read_u8(&reader, UINT8_C(1))
            || !cm_decl_read_u8(&reader, UINT8_C(2))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &trait_value->owner_module)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_string(&reader, &trait_value->name)
            || !cm_decl_read_item_visibility(&reader,
                &trait_value->visibility)
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->source_ordinal) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->generic_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->generic_count) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->predicate_scope_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->predicate_scope_count) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->predicate_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->predicate_count) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->outlives_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->outlives_count) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->associated_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->associated_count) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &trait_value->safety)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &trait_value->flags)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u16(&reader,
                &trait_value->compiler_flags) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->supertrait_count) != CM_HIR_METADATA_OK
            || trait_value->supertrait_count
                > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES) return 0;
        trait_value->supertraits = trait_value->supertrait_count == 0u ? NULL
            : (CmHirDeclarationSupertrait *)cm_alloc_zeroed(
                trait_value->supertrait_count,
                sizeof(*trait_value->supertraits));
        for (child = 0u; child < trait_value->supertrait_count; ++child) {
            CmHirDeclarationSupertrait *supertrait =
                &trait_value->supertraits[child];
            uint32_t argument;
            if (cm_hir_metadata_read_u8(&reader,
                    &supertrait->modifier) != CM_HIR_METADATA_OK
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u16(&reader, UINT16_C(0))
                || cm_hir_metadata_read_u32(&reader,
                    &supertrait->trait_local) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &supertrait->argument_count) != CM_HIR_METADATA_OK
                || supertrait->argument_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_arguments,
                    supertrait->argument_count, &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES)
                return 0;
            supertrait->argument_types = supertrait->argument_count == 0u
                ? NULL : (uint32_t *)cm_alloc(
                    (size_t)supertrait->argument_count * sizeof(uint32_t));
            for (argument = 0u; argument < supertrait->argument_count;
                    ++argument)
                if (cm_hir_metadata_read_u32(&reader,
                        &supertrait->argument_types[argument])
                        != CM_HIR_METADATA_OK) return 0;
            if (!cm_decl_read_u32(&reader, UINT32_C(0))
                || !cm_decl_read_u32(&reader, UINT32_C(0))) return 0;
        }
        if ((trait_value->flags
                & CM_HIR_DECL_TRAIT_HAS_DIAGNOSTIC_ITEM) != 0u
            && !cm_decl_read_string(&reader,
                &trait_value->diagnostic_item)) return 0;
        if ((trait_value->flags & CM_HIR_DECL_TRAIT_HAS_LANG_ITEM) != 0u
            && !cm_decl_read_string(&reader,
                &trait_value->lang_item)) return 0;
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_parse_associated(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    size_t total_parameters;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader,
            CM_HIR_DECL_METADATA_MAX_ASSOCIATED_ITEMS,
            &metadata->associated_count)) return 0;
    metadata->associated_items = metadata->associated_count == 0u ? NULL
        : (CmHirDeclarationAssociatedItem *)cm_alloc_zeroed(
            metadata->associated_count,
            sizeof(*metadata->associated_items));
    total_parameters = 0u;
    for (index = 0u; index < metadata->associated_count; ++index) {
        CmHirDeclarationAssociatedItem *associated =
            &metadata->associated_items[index];
        uint32_t child;
        if (cm_hir_metadata_read_u8(&reader, &associated->kind)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &associated->parent_kind)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader,
                &associated->parent_local) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &associated->implemented_associated_local)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_string(&reader, &associated->name)
            || !cm_decl_read_item_visibility(&reader,
                &associated->visibility)
            || cm_hir_metadata_read_u32(&reader,
                &associated->source_ordinal) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader,
                &associated->is_specializable) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &associated->flags)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader,
                &associated->generic_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &associated->generic_count) != CM_HIR_METADATA_OK
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || cm_hir_metadata_read_u32(&reader,
                &associated->predicate_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &associated->predicate_count) != CM_HIR_METADATA_OK
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0)))
            return 0;
        if (associated->kind == CM_HIR_DECL_ASSOCIATED_METHOD) {
            if (cm_hir_metadata_read_u8(&reader, &associated->receiver)
                    != CM_HIR_METADATA_OK
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u16(&reader, UINT16_C(0))
                || cm_hir_metadata_read_u32(&reader,
                    &associated->parameter_count) != CM_HIR_METADATA_OK
                || associated->parameter_count == 0u
                || associated->parameter_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_parameters,
                    associated->parameter_count, &total_parameters)
                || total_parameters > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES)
                return 0;
            associated->parameter_types = (uint32_t *)cm_alloc(
                (size_t)associated->parameter_count * sizeof(uint32_t));
            for (child = 0u; child < associated->parameter_count; ++child) {
                if (cm_hir_metadata_read_u32(&reader,
                        &associated->parameter_types[child])
                        != CM_HIR_METADATA_OK) return 0;
            }
            if (cm_hir_metadata_read_u32(&reader,
                    &associated->return_type) != CM_HIR_METADATA_OK
                || !cm_decl_read_string(&reader, &associated->abi)
                || cm_hir_metadata_read_u8(&reader,
                    &associated->safety) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader,
                    &associated->is_const) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader,
                    &associated->is_async) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader,
                    &associated->is_variadic) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader,
                    &associated->has_default_body) != CM_HIR_METADATA_OK
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u16(&reader, UINT16_C(0))) return 0;
        } else if (associated->kind != CM_HIR_DECL_ASSOCIATED_TYPE) {
            return 0;
        }
        if ((associated->flags
                & CM_HIR_DECL_ASSOCIATED_HAS_LANG_ITEM) != 0u
            && !cm_decl_read_string(&reader,
                &associated->lang_item)) return 0;
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_parse_zero(const CmHirMetadataSection *section)
{
    CmHirMetadataReader reader;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    return cm_decl_read_u32(&reader, UINT32_C(0))
        && cm_decl_reader_done(&reader);
}

static int cm_decl_parse_generics(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader, CM_HIR_DECL_METADATA_MAX_GENERICS,
            &metadata->generic_count)) return 0;
    metadata->generics = metadata->generic_count == 0u ? NULL
        : (CmHirDeclarationGeneric *)cm_alloc_zeroed(metadata->generic_count,
            sizeof(CmHirDeclarationGeneric));
    for (index = 0u; index < metadata->generic_count; ++index) {
        CmHirDeclarationGeneric *generic;
        generic = &metadata->generics[index];
        if (cm_hir_metadata_read_u8(&reader, &generic->owner_kind)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &generic->owner_local)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &generic->index)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &generic->kind)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &generic->is_relaxed_sized)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || !cm_decl_read_string(&reader, &generic->name)
            || cm_hir_metadata_read_u8(&reader, &generic->has_default)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &generic->declared_type)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u32(&reader, UINT32_C(0))) return 0;
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_parse_types(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    size_t total_arguments;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader, CM_HIR_DECL_METADATA_MAX_TYPES,
            &metadata->type_count)) return 0;
    metadata->types = metadata->type_count == 0u ? NULL
        : (CmHirDeclarationType *)cm_alloc_zeroed(metadata->type_count,
            sizeof(CmHirDeclarationType));
    total_arguments = 0u;
    for (index = 0u; index < metadata->type_count; ++index) {
        CmHirDeclarationType *type;
        type = &metadata->types[index];
        if (cm_hir_metadata_read_u8(&reader, &type->kind)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &type->array_length_kind)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u16(&reader, UINT16_C(0))) return 0;
        if (type->kind != CM_HIR_DECL_TYPE_ARRAY
            && type->array_length_kind != 0u) return 0;
        if (type->kind == CM_HIR_DECL_TYPE_PRIMITIVE) {
            if (cm_hir_metadata_read_u8(&reader, &type->primitive)
                    != CM_HIR_METADATA_OK
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u16(&reader, UINT16_C(0))) return 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_GENERIC) {
            if (cm_hir_metadata_read_u32(&reader, &type->generic_local)
                    != CM_HIR_METADATA_OK) return 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_NAMED_ADT) {
            if (cm_hir_metadata_read_u32(&reader, &type->item_local)
                    != CM_HIR_METADATA_OK) return 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_SELF) {
            if (cm_hir_metadata_read_u32(&reader, &type->self_trait_local)
                    != CM_HIR_METADATA_OK) return 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_SLICE) {
            if (cm_hir_metadata_read_u32(&reader, &type->child_type)
                    != CM_HIR_METADATA_OK) return 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_RAW_POINTER) {
            if (cm_hir_metadata_read_u8(&reader, &type->mutability)
                    != CM_HIR_METADATA_OK
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u16(&reader, UINT16_C(0))
                || cm_hir_metadata_read_u32(&reader, &type->child_type)
                    != CM_HIR_METADATA_OK) return 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_REFERENCE) {
            if (cm_hir_metadata_read_u8(&reader, &type->region.kind)
                    != CM_HIR_METADATA_OK
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u16(&reader, UINT16_C(0))
                || cm_hir_metadata_read_u32(&reader,
                    &type->region.generic_local) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &type->region.binder_index) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader, &type->mutability)
                    != CM_HIR_METADATA_OK
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u16(&reader, UINT16_C(0))
                || cm_hir_metadata_read_u32(&reader, &type->child_type)
                    != CM_HIR_METADATA_OK) return 0;
        } else if (type->kind
                == CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION) {
            uint32_t child;
            if (cm_hir_metadata_read_u32(&reader, &type->item_local)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &type->argument_count) != CM_HIR_METADATA_OK
                || type->argument_count == 0u
                || type->argument_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_arguments, type->argument_count,
                    &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES)
                return 0;
            type->argument_types = (uint32_t *)cm_alloc(
                (size_t)type->argument_count * sizeof(uint32_t));
            for (child = 0u; child < type->argument_count; ++child) {
                if (cm_hir_metadata_read_u32(&reader,
                        &type->argument_types[child])
                        != CM_HIR_METADATA_OK) return 0;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_TUPLE) {
            uint32_t child;
            if (cm_hir_metadata_read_u32(&reader, &type->element_count)
                    != CM_HIR_METADATA_OK
                || type->element_count == 0u
                || type->element_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_arguments, type->element_count,
                    &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES)
                return 0;
            type->element_types = (uint32_t *)cm_alloc(
                (size_t)type->element_count * sizeof(uint32_t));
            for (child = 0u; child < type->element_count; ++child) {
                if (cm_hir_metadata_read_u32(&reader,
                        &type->element_types[child])
                        != CM_HIR_METADATA_OK) return 0;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_ARRAY) {
            if (!cm_size_add(total_arguments, 2u, &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || cm_hir_metadata_read_u32(&reader, &type->child_type)
                    != CM_HIR_METADATA_OK
                || (type->array_length_kind
                        != CM_HIR_DECL_ARRAY_LENGTH_SCALAR
                    && type->array_length_kind
                        != CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER))
                return 0;
            if (type->array_length_kind
                    == CM_HIR_DECL_ARRAY_LENGTH_CONST_PARAMETER) {
                if (cm_hir_metadata_read_u32(&reader,
                        &type->array_length_generic_local)
                        != CM_HIR_METADATA_OK
                    || !cm_decl_read_u64(&reader, UINT64_C(0))
                    || !cm_decl_read_u64(&reader, UINT64_C(0))) return 0;
            } else if (cm_hir_metadata_read_u32(&reader,
                    &type->array_length_type) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u64(&reader,
                    &type->array_length_low_bits) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u64(&reader,
                    &type->array_length_high_bits) != CM_HIR_METADATA_OK) {
                return 0;
            }
        } else if (type->kind == CM_HIR_DECL_TYPE_PROJECTION) {
            uint32_t child;
            if (cm_hir_metadata_read_u32(&reader,
                    &type->projection_self_type) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &type->projection_trait_local) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &type->projection_associated_local)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &type->projection_argument_count)
                    != CM_HIR_METADATA_OK
                || type->projection_argument_count == 0u
                || type->projection_argument_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_arguments,
                    type->projection_argument_count, &total_arguments)
                || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES)
                return 0;
            type->projection_argument_types = (uint32_t *)cm_alloc(
                (size_t)type->projection_argument_count
                    * sizeof(uint32_t));
            for (child = 0u; child < type->projection_argument_count;
                    ++child)
                if (cm_hir_metadata_read_u32(&reader,
                        &type->projection_argument_types[child])
                        != CM_HIR_METADATA_OK) return 0;
        } else {
            return 0;
        }
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_parse_items(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    size_t total_fields;
    size_t total_variants;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader, CM_HIR_DECL_METADATA_MAX_ITEMS,
            &metadata->item_count)) return 0;
    metadata->items = metadata->item_count == 0u ? NULL
        : (CmHirDeclarationItem *)cm_alloc_zeroed(metadata->item_count,
            sizeof(CmHirDeclarationItem));
    total_fields = 0u;
    total_variants = 0u;
    for (index = 0u; index < metadata->item_count; ++index) {
        CmHirDeclarationItem *item;
        item = &metadata->items[index];
        if (cm_hir_metadata_read_u8(&reader, &item->kind)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &item->owner_module)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_string(&reader, &item->name)
            || !cm_decl_read_item_visibility(&reader, &item->visibility)
            || cm_hir_metadata_read_u32(&reader, &item->source_ordinal)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &item->generic_start)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &item->generic_count)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &item->predicate_scope_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &item->predicate_scope_count) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &item->predicate_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &item->predicate_count) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &item->outlives_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &item->outlives_count) != CM_HIR_METADATA_OK) return 0;
        if (item->kind == CM_HIR_DECL_ITEM_STRUCT
                || item->kind == CM_HIR_DECL_ITEM_UNION) {
            uint32_t child;
            if (cm_hir_metadata_read_u8(&reader,
                    &item->aggregate_form) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader,
                    &item->aggregate_repr) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u16(&reader,
                    &item->aggregate_flags) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &item->field_count) != CM_HIR_METADATA_OK
                || item->field_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_FIELDS
                || !cm_size_add(total_fields, item->field_count,
                    &total_fields)
                || total_fields > CM_HIR_DECL_METADATA_MAX_FIELDS)
                return 0;
            item->fields = item->field_count == 0u ? NULL
                : (CmHirDeclarationField *)cm_alloc_zeroed(
                    item->field_count, sizeof(*item->fields));
            for (child = 0u; child < item->field_count; ++child) {
                CmHirDeclarationField *field = &item->fields[child];
                if (!cm_decl_read_string(&reader, &field->name)
                    || !cm_decl_read_item_visibility(&reader,
                        &field->visibility)
                    || cm_hir_metadata_read_u32(&reader,
                        &field->source_ordinal) != CM_HIR_METADATA_OK
                    || cm_hir_metadata_read_u32(&reader,
                        &field->type_local) != CM_HIR_METADATA_OK)
                    return 0;
                if (item->aggregate_form == CM_HIR_DECL_AGGREGATE_TUPLE
                    && field->name.length == 0u) {
                    cm_free(field->name.data);
                    field->name.data = NULL;
                }
            }
            if ((item->aggregate_flags
                    & CM_HIR_DECL_AGGREGATE_HAS_LANG_ITEM) != 0u
                && !cm_decl_read_string(&reader, &item->lang_item))
                return 0;
            if ((item->aggregate_flags
                    & CM_HIR_DECL_AGGREGATE_HAS_DIAGNOSTIC_ITEM) != 0u
                && !cm_decl_read_string(&reader,
                    &item->diagnostic_item)) return 0;
        } else if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t child;
            if (cm_hir_metadata_read_u8(&reader,
                    &item->enum_repr_primitive) != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader, &item->enum_flags)
                    != CM_HIR_METADATA_OK
                || !cm_decl_read_u16(&reader, UINT16_C(0))
                || cm_hir_metadata_read_u32(&reader,
                    &item->variant_count) != CM_HIR_METADATA_OK
                || item->variant_count == 0u
                || item->variant_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_VARIANTS
                || !cm_size_add(total_variants, item->variant_count,
                    &total_variants)
                || total_variants > CM_HIR_DECL_METADATA_MAX_VARIANTS)
                return 0;
            item->variants = (CmHirDeclarationVariant *)cm_alloc_zeroed(
                item->variant_count, sizeof(*item->variants));
            for (child = 0u; child < item->variant_count; ++child) {
                CmHirDeclarationVariant *variant = &item->variants[child];
                uint32_t field_index;
                if (cm_hir_metadata_read_u8(&reader, &variant->kind)
                        != CM_HIR_METADATA_OK
                    || cm_hir_metadata_read_u8(&reader,
                        &variant->discriminant_primitive)
                        != CM_HIR_METADATA_OK
                    || cm_hir_metadata_read_u16(&reader, &variant->flags)
                        != CM_HIR_METADATA_OK
                    || !cm_decl_read_string(&reader, &variant->name)
                    || cm_hir_metadata_read_u32(&reader,
                        &variant->source_ordinal) != CM_HIR_METADATA_OK
                    || cm_hir_metadata_read_u64(&reader,
                        &variant->discriminant_low) != CM_HIR_METADATA_OK
                    || cm_hir_metadata_read_u64(&reader,
                        &variant->discriminant_high) != CM_HIR_METADATA_OK)
                    return 0;
                if (variant->kind == CM_HIR_DECL_VARIANT_TUPLE) {
                    if (cm_hir_metadata_read_u32(&reader,
                            &variant->field_count) != CM_HIR_METADATA_OK
                        || variant->field_count == 0u
                        || variant->field_count
                            > (uint32_t)CM_HIR_DECL_METADATA_MAX_FIELDS
                        || !cm_size_add(total_fields,
                            variant->field_count, &total_fields)
                        || total_fields
                            > CM_HIR_DECL_METADATA_MAX_FIELDS) return 0;
                    variant->fields =
                        (CmHirDeclarationVariantField *)cm_alloc_zeroed(
                            variant->field_count,
                            sizeof(*variant->fields));
                    for (field_index = 0u;
                            field_index < variant->field_count;
                            ++field_index) {
                        if (cm_hir_metadata_read_u32(&reader,
                                &variant->fields[field_index]
                                    .source_ordinal)
                                != CM_HIR_METADATA_OK
                            || cm_hir_metadata_read_u32(&reader,
                                &variant->fields[field_index].type_local)
                                != CM_HIR_METADATA_OK) return 0;
                    }
                }
                if ((variant->flags
                        & CM_HIR_DECL_VARIANT_HAS_LANG_ITEM) != 0u
                    && !cm_decl_read_string(&reader,
                        &variant->lang_item)) return 0;
            }
            if (item->enum_repr_primitive == CM_HIR_DECL_ENUM_REPR_RUST
                && !cm_decl_read_string(&reader,
                    &item->diagnostic_item)) return 0;
            if ((item->enum_flags
                    & CM_HIR_DECL_ENUM_HAS_LANG_ITEM) != 0u
                && !cm_decl_read_string(&reader,
                    &item->enum_lang_item)) return 0;
        } else if (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS) {
            if (cm_hir_metadata_read_u32(&reader,
                    &item->alias_target_type) != CM_HIR_METADATA_OK) return 0;
        } else {
            return 0;
        }
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_parse_values(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    size_t total_parameters;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader, CM_HIR_DECL_METADATA_MAX_VALUES,
            &metadata->value_count)) return 0;
    metadata->values = metadata->value_count == 0u ? NULL
        : (CmHirDeclarationValue *)cm_alloc_zeroed(metadata->value_count,
            sizeof(CmHirDeclarationValue));
    total_parameters = 0u;
    for (index = 0u; index < metadata->value_count; ++index) {
        CmHirDeclarationValue *value;
        uint32_t child;
        uint8_t kind;
        value = &metadata->values[index];
        if (cm_hir_metadata_read_u8(&reader, &kind) != CM_HIR_METADATA_OK
            || (kind != CM_HIR_DECL_VALUE_FUNCTION
                && kind != CM_HIR_DECL_VALUE_CONST
                && kind != CM_HIR_DECL_VALUE_STATIC)
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &value->owner_module)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_string(&reader, &value->name)
            || !cm_decl_read_visibility(&reader)
            || cm_hir_metadata_read_u32(&reader, &value->source_ordinal)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &value->generic_start)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &value->generic_count)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || cm_hir_metadata_read_u32(&reader, &value->predicate_start)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &value->predicate_count)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))) return 0;
        value->kind = kind;
        if (kind == CM_HIR_DECL_VALUE_FUNCTION) {
            if (!cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || cm_hir_metadata_read_u32(&reader,
                    &value->parameter_count) != CM_HIR_METADATA_OK
                || value->parameter_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
                || !cm_size_add(total_parameters, value->parameter_count,
                    &total_parameters)
                || total_parameters
                    > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES) return 0;
            value->parameter_types = value->parameter_count == 0u ? NULL
                : (uint32_t *)cm_alloc((size_t)value->parameter_count
                    * sizeof(uint32_t));
            for (child = 0u; child < value->parameter_count; ++child) {
                if (cm_hir_metadata_read_u32(&reader,
                        &value->parameter_types[child])
                        != CM_HIR_METADATA_OK) return 0;
            }
            if (cm_hir_metadata_read_u32(&reader, &value->return_type)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader, &value->has_body)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader, &value->is_const)
                    != CM_HIR_METADATA_OK
                || !cm_decl_read_u16(&reader, UINT16_C(0))) return 0;
        } else {
            if (cm_hir_metadata_read_u32(&reader, &value->declared_type)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader, &value->mutability)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u8(&reader, &value->has_body)
                    != CM_HIR_METADATA_OK
                || !cm_decl_read_u16(&reader, UINT16_C(0))) {
                return 0;
            }
        }
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_parse_predicates(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t count;
    size_t outlives_count;
    size_t index;
    size_t total_arguments;
    size_t total_equalities;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_u32(&reader, UINT32_C(0))
        || !cm_decl_read_count(&reader,
            CM_HIR_DECL_METADATA_MAX_PREDICATES, &count)
        || !cm_decl_read_count(&reader,
            CM_HIR_DECL_METADATA_MAX_PREDICATES, &outlives_count)
        || count > CM_HIR_DECL_METADATA_MAX_PREDICATES
                - outlives_count) return 0;
    metadata->predicate_count = count;
    metadata->predicates = count == 0u ? NULL
        : (CmHirDeclarationPredicate *)cm_alloc_zeroed(count,
            sizeof(CmHirDeclarationPredicate));
    total_arguments = 0u;
    total_equalities = 0u;
    for (index = 0u; index < count; ++index) {
        CmHirDeclarationPredicate *predicate;
        uint32_t owner_local;
        uint32_t child;
        predicate = &metadata->predicates[index];
        if (!cm_decl_read_u8(&reader, UINT8_C(4))
            || cm_hir_metadata_read_u8(&reader, &predicate->owner_kind)
                != CM_HIR_METADATA_OK
            || (predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_VALUE
                && predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED
                && predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_NOMINAL
                && predicate->owner_kind
                    != CM_HIR_DECL_PREDICATE_OWNER_ITEM)
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &owner_local)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &predicate->ordinal)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &predicate->subject_type)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &predicate->trait_local)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &predicate->argument_count)
                != CM_HIR_METADATA_OK
            || predicate->argument_count
                > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || !cm_size_add(total_arguments, predicate->argument_count,
                &total_arguments)
            || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES)
            return 0;
        if (predicate->owner_kind == CM_HIR_DECL_PREDICATE_OWNER_VALUE)
            predicate->owner_value = owner_local;
        else if (predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_ASSOCIATED)
            predicate->owner_associated = owner_local;
        else if (predicate->owner_kind
                == CM_HIR_DECL_PREDICATE_OWNER_NOMINAL)
            predicate->owner_nominal = owner_local;
        else
            predicate->owner_item = owner_local;
        predicate->argument_types = predicate->argument_count == 0u ? NULL
            : (uint32_t *)cm_alloc((size_t)predicate->argument_count
                * sizeof(uint32_t));
        for (child = 0u; child < predicate->argument_count; ++child) {
            if (cm_hir_metadata_read_u32(&reader,
                    &predicate->argument_types[child])
                    != CM_HIR_METADATA_OK) return 0;
        }
        if (cm_hir_metadata_read_u32(&reader,
                &predicate->equality_count) != CM_HIR_METADATA_OK
            || predicate->equality_count
                > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || !cm_size_add(total_equalities, predicate->equality_count,
                &total_equalities)
            || total_equalities > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES)
            return 0;
        predicate->equalities = predicate->equality_count == 0u ? NULL
            : (CmHirDeclarationPredicateEquality *)cm_alloc(
                (size_t)predicate->equality_count
                    * sizeof(*predicate->equalities));
        for (child = 0u; child < predicate->equality_count; ++child) {
            if (cm_hir_metadata_read_u32(&reader,
                    &predicate->equalities[child].associated_local)
                    != CM_HIR_METADATA_OK
                || cm_hir_metadata_read_u32(&reader,
                    &predicate->equalities[child].value_type)
                    != CM_HIR_METADATA_OK) return 0;
        }
        {
            uint8_t modifier_tag;
            if (!cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || cm_hir_metadata_read_u8(&reader, &modifier_tag)
                != CM_HIR_METADATA_OK
            || modifier_tag < UINT8_C(1) || modifier_tag > UINT8_C(3)
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))) return 0;
            predicate->modifier = (uint8_t)(modifier_tag - UINT8_C(1));
        }
    }
    metadata->outlives_predicate_count = outlives_count;
    metadata->outlives_predicates = outlives_count == 0u ? NULL
        : (CmHirDeclarationOutlivesPredicate *)cm_alloc_zeroed(
            outlives_count, sizeof(*metadata->outlives_predicates));
    for (index = 0u; index < outlives_count; ++index) {
        CmHirDeclarationOutlivesPredicate *predicate =
            &metadata->outlives_predicates[index];
        if (!cm_decl_read_u8(&reader, UINT8_C(5))
            || cm_hir_metadata_read_u8(&reader, &predicate->owner_kind)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &predicate->owner_local)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &predicate->ordinal)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &predicate->subject_type)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &predicate->bound.kind)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader,
                &predicate->bound.generic_local) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &predicate->bound.binder_index) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader, &predicate->scope)
                != CM_HIR_METADATA_OK) return 0;
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_parse_namespace(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t index;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader,
            CM_HIR_DECL_METADATA_MAX_NAMESPACE_ENTRIES,
            &metadata->namespace_count)) return 0;
    metadata->namespace_entries = metadata->namespace_count == 0u ? NULL
        : (CmHirDeclarationNamespaceEntry *)cm_alloc_zeroed(
            metadata->namespace_count,
            sizeof(CmHirDeclarationNamespaceEntry));
    for (index = 0u; index < metadata->namespace_count; ++index) {
        CmHirDeclarationNamespaceEntry *entry;
        entry = &metadata->namespace_entries[index];
        if (cm_hir_metadata_read_u32(&reader, &entry->owner_module)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &entry->namespace_kind)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &entry->target_kind)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || !cm_decl_read_string(&reader, &entry->name)
            || cm_hir_metadata_read_u32(&reader, &entry->target_local)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_visibility(&reader)
            || cm_hir_metadata_read_u32(&reader, &entry->export_ordinal)
                != CM_HIR_METADATA_OK) return 0;
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_sections_match(
    const CmHirDeclarationMetadata *metadata,
    const CmHirMetadataSection original[CM_DECL_SECTION_COUNT])
{
    CmByteBuf rebuilt[CM_DECL_SECTION_COUNT];
    CmDeclFamily families[CM_DECL_FAMILY_COUNT];
    CmHirDeclarationMetadataStatus status;
    size_t index;
    int valid;
    status = cm_decl_build_sections(metadata, rebuilt);
    if (status == CM_HIR_DECL_METADATA_OK)
        status = cm_decl_build_manifest(metadata, rebuilt, families);
    valid = status == CM_HIR_DECL_METADATA_OK;
    for (index = 0u; index < CM_DECL_SECTION_COUNT; ++index) {
        if (valid && (rebuilt[index].len != original[index].length
            || (rebuilt[index].len != 0u
                && memcmp(rebuilt[index].data, original[index].data,
                    rebuilt[index].len) != 0))) valid = 0;
    }
    for (index = 0u; index < CM_DECL_SECTION_COUNT; ++index)
        cm_byte_buf_destroy(&rebuilt[index]);
    return valid;
}

CmHirDeclarationMetadataStatus cm_hir_declaration_metadata_decode(
    const void *encoded, size_t encoded_length,
    CmHirDeclarationMetadata *output)
{
    CmHirMetadataEnvelope envelope;
    CmHirMetadataReader reader;
    CmHirMetadataSection sections[CM_DECL_SECTION_COUNT];
    CmDeclFamily families[CM_DECL_FAMILY_COUNT];
    CmHirDeclarationMetadata candidate;
    CmHirMetadataStatus wire_status;
    CmHirDeclarationMetadataStatus status;
    size_t index;
    if (output == NULL || (encoded_length != 0u && encoded == NULL))
        return CM_HIR_DECL_METADATA_INVALID_ARGUMENT;
    wire_status = cm_hir_metadata_decode_envelope_version(encoded,
        encoded_length, CM_HIR_DECL_METADATA_MAJOR,
        CM_HIR_DECL_METADATA_MINOR, &envelope);
    if (wire_status != CM_HIR_METADATA_OK) {
        if (wire_status == CM_HIR_METADATA_UNSUPPORTED_VERSION)
            return CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR;
        if (wire_status == CM_HIR_METADATA_LIMIT_EXCEEDED
            || wire_status == CM_HIR_METADATA_LENGTH_OVERFLOW)
            return CM_HIR_DECL_METADATA_LIMIT_EXCEEDED;
        return CM_HIR_DECL_METADATA_INVALID_FORMAT;
    }
    cm_hir_metadata_reader_init(&reader, envelope.payload,
        envelope.payload_length);
    for (index = 0u; index < CM_DECL_SECTION_COUNT; ++index) {
        if (cm_hir_metadata_read_section(&reader, &sections[index])
                != CM_HIR_METADATA_OK
            || memcmp(sections[index].tag, cm_decl_tags[index], 4u) != 0)
            return CM_HIR_DECL_METADATA_INVALID_FORMAT;
    }
    if (cm_hir_metadata_read_section(&reader, &sections[0])
            != CM_HIR_METADATA_DONE) return CM_HIR_DECL_METADATA_INVALID_FORMAT;
    cm_hir_declaration_metadata_init(&candidate);
    candidate.owns_storage = 1;
    memset(families, 0, sizeof(families));
    if (!cm_decl_parse_manifest(&sections[1], &candidate, families)
        || !cm_decl_parse_crat(&sections[0], &candidate)
        || !cm_decl_parse_modules(&sections[2], &candidate)
        || !cm_decl_parse_traits(&sections[3], &candidate)
        || !cm_decl_parse_associated(&sections[4], &candidate)
        || !cm_decl_parse_generics(&sections[5], &candidate)
        || !cm_decl_parse_types(&sections[6], &candidate)
        || !cm_decl_parse_items(&sections[7], &candidate)
        || !cm_decl_parse_values(&sections[8], &candidate)
        || !cm_decl_parse_predicates(&sections[9], &candidate)
        || !cm_decl_parse_zero(&sections[10])
        || !cm_decl_parse_namespace(&sections[11], &candidate)) {
        cm_hir_declaration_metadata_destroy(&candidate);
        return CM_HIR_DECL_METADATA_INVALID_FORMAT;
    }
    status = cm_hir_declaration_metadata_validate(&candidate);
    if (status != CM_HIR_DECL_METADATA_OK
        || !cm_decl_sections_match(&candidate, sections)) {
        cm_hir_declaration_metadata_destroy(&candidate);
        return status == CM_HIR_DECL_METADATA_LIMIT_EXCEEDED ? status
            : CM_HIR_DECL_METADATA_INVALID_FORMAT;
    }
    cm_hir_declaration_metadata_destroy(output);
    *output = candidate;
    return CM_HIR_DECL_METADATA_OK;
}

const char *cm_hir_declaration_metadata_status_name(
    CmHirDeclarationMetadataStatus status)
{
    switch (status) {
    case CM_HIR_DECL_METADATA_OK: return "ok";
    case CM_HIR_DECL_METADATA_INVALID_ARGUMENT: return "invalid argument";
    case CM_HIR_DECL_METADATA_LIMIT_EXCEEDED: return "limit exceeded";
    case CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR:
        return "unsupported descriptor";
    case CM_HIR_DECL_METADATA_INVALID_FORMAT: return "invalid format";
    }
    return "unknown";
}
