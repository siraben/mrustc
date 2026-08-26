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

static int cm_decl_primitive(uint8_t value)
{
    return value >= (uint8_t)CM_HIR_DECL_PRIMITIVE_UNIT
        && value <= (uint8_t)CM_HIR_DECL_PRIMITIVE_F64;
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
    for (index = 0u; index < metadata->trait_count; ++index)
        cm_decl_free_string(&metadata->traits[index].name);
    for (index = 0u; index < metadata->generic_count; ++index)
        cm_decl_free_string(&metadata->generics[index].name);
    for (index = 0u; index < metadata->type_count; ++index)
        cm_free(metadata->types[index].argument_types);
    for (index = 0u; index < metadata->item_count; ++index) {
        uint32_t child;
        cm_decl_free_string(&metadata->items[index].name);
        for (child = 0u; metadata->items[index].variants != NULL
                && child < metadata->items[index].variant_count;
                ++child)
            cm_decl_free_string(&metadata->items[index].variants[child].name);
        cm_free(metadata->items[index].variants);
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        cm_decl_free_string(&metadata->values[index].name);
        cm_free(metadata->values[index].parameter_types);
    }
    for (index = 0u; index < metadata->predicate_count; ++index)
        cm_free(metadata->predicates[index].argument_types);
    for (index = 0u; index < metadata->namespace_count; ++index)
        cm_decl_free_string(&metadata->namespace_entries[index].name);
    cm_free(metadata->cfgs);
    cm_free(metadata->modules);
    cm_free(metadata->traits);
    cm_free(metadata->generics);
    cm_free(metadata->types);
    cm_free(metadata->items);
    cm_free(metadata->values);
    cm_free(metadata->predicates);
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

static int cm_decl_validate_generics(
    const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t owner_index;
    unsigned char *seen;
    if (metadata->generic_count != 0u && metadata->generics == NULL) return 0;
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
            || generic->kind != CM_HIR_DECL_GENERIC_TYPE
            || generic->is_relaxed_sized > 1u
            || !cm_decl_identifier(generic->name)) {
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
    if (metadata->trait_count != 0u && metadata->traits == NULL) return 0;
    for (index = 0u; index < metadata->trait_count; ++index) {
        const CmHirDeclarationTrait *trait_value;
        trait_value = &metadata->traits[index];
        if (trait_value->owner_module == 0u
            || (size_t)trait_value->owner_module > metadata->module_count
            || !cm_decl_identifier(trait_value->name)) return 0;
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

static int cm_decl_variant_name_compare(const void *left_value,
    const void *right_value)
{
    const CmHirDeclarationString *left =
        (const CmHirDeclarationString *)left_value;
    const CmHirDeclarationString *right =
        (const CmHirDeclarationString *)right_value;
    return cm_decl_string_compare(*left, *right);
}

static int cm_decl_validate_items(const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t total_variants;
    if (metadata->item_count != 0u && metadata->items == NULL) return 0;
    total_variants = 0u;
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item;
        item = &metadata->items[index];
        if ((item->kind != CM_HIR_DECL_ITEM_STRUCT
                && item->kind != CM_HIR_DECL_ITEM_ENUM
                && item->kind != CM_HIR_DECL_ITEM_TYPE_ALIAS)
            || item->owner_module == 0u
            || (size_t)item->owner_module > metadata->module_count
            || !cm_decl_identifier(item->name)
            || item->visibility.kind != CM_HIR_DECL_VISIBILITY_PUBLIC
            || item->visibility.restriction_module != 0u
            || (item->kind != CM_HIR_DECL_ITEM_TYPE_ALIAS
                && item->alias_target_type != 0u)
            || (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
                && !cm_decl_type_local(metadata,
                    item->alias_target_type))
            || ((item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
                    || item->kind == CM_HIR_DECL_ITEM_ENUM)
                && (item->generic_start != 0u
                    || item->generic_count != 0u))
            || (item->kind != CM_HIR_DECL_ITEM_ENUM
                && (item->enum_repr_primitive != 0u
                    || item->variant_count != 0u
                    || item->variants != NULL))) return 0;
        if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            CmHirDeclarationString *names;
            unsigned char discriminants[256];
            uint32_t child;
            if (item->enum_repr_primitive != CM_HIR_DECL_PRIMITIVE_U8
                || item->variant_count == 0u
                || item->variant_count
                    > (uint32_t)CM_HIR_DECL_METADATA_MAX_VARIANTS
                || item->variants == NULL
                || !cm_size_add(total_variants, item->variant_count,
                    &total_variants)
                || total_variants > CM_HIR_DECL_METADATA_MAX_VARIANTS)
                return 0;
            names = (CmHirDeclarationString *)cm_alloc(
                (size_t)item->variant_count * sizeof(*names));
            memset(discriminants, 0, sizeof(discriminants));
            for (child = 0u; child < item->variant_count; ++child) {
                const CmHirDeclarationVariant *variant =
                    &item->variants[child];
                if (variant->kind != CM_HIR_DECL_VARIANT_UNIT
                    || !cm_decl_identifier(variant->name)
                    || (child != 0u && item->variants[child - 1u]
                        .source_ordinal >= variant->source_ordinal)
                    || variant->discriminant_primitive
                        != CM_HIR_DECL_PRIMITIVE_ISIZE
                    || variant->discriminant_high != UINT64_C(0)
                    || variant->discriminant_low > UINT64_C(255)
                    || discriminants[(size_t)variant->discriminant_low]) {
                    cm_free(names);
                    return 0;
                }
                discriminants[(size_t)variant->discriminant_low] = 1u;
                names[child] = variant->name;
            }
            qsort(names, item->variant_count, sizeof(*names),
                cm_decl_variant_name_compare);
            for (child = 1u; child < item->variant_count; ++child) {
                if (cm_decl_string_equal(names[child - 1u], names[child])) {
                    cm_free(names);
                    return 0;
                }
            }
            cm_free(names);
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
    return 0;
}

static int cm_decl_type_common_zero(const CmHirDeclarationType *type)
{
    return type->primitive == 0u && type->generic_local == 0u
        && type->item_local == 0u && type->child_type == 0u
        && type->self_trait_local == 0u && type->mutability == 0u
        && type->region.kind == 0u && type->region.generic_local == 0u
        && type->region.binder_index == 0u
        && type->argument_count == 0u && type->argument_types == NULL;
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
                || !cm_decl_type_common_zero(&copy)) valid = 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_NAMED_ADT) {
            CmHirDeclarationType copy = *type;
            copy.item_local = 0u;
            if (type->item_local == 0u
                || (size_t)type->item_local > metadata->item_count
                || (metadata->items[type->item_local - 1u].kind
                        != CM_HIR_DECL_ITEM_STRUCT
                    && metadata->items[type->item_local - 1u].kind
                        != CM_HIR_DECL_ITEM_ENUM)
                || metadata->items[type->item_local - 1u].generic_count != 0u
                || !cm_decl_type_common_zero(&copy)) valid = 0;
        } else if (type->kind == CM_HIR_DECL_TYPE_SELF) {
            /* Reserved on wire, but this slice has no honest SELF root. */
            valid = 0;
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
                    if (type->region.kind != CM_HIR_DECL_REGION_STATIC
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
            if (item->kind != CM_HIR_DECL_ITEM_STRUCT
                || item->generic_count != type->argument_count) {
                valid = 0;
                continue;
            }
            maximum_depth = 0u;
            for (child = 0u; child < type->argument_count; ++child) {
                uint32_t argument = type->argument_types[child];
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
            || value->generic_count == 0u
            || value->predicate_count == 0u
            || !cm_decl_range(value->predicate_start,
                value->predicate_count, metadata->predicate_count)
            || value->parameter_count
                > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || !cm_size_add(total_parameters, value->parameter_count,
                &total_parameters)
            || total_parameters > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || ((value->parameter_count == 0u)
                != (value->parameter_types == NULL))
            || !cm_decl_type_local(metadata, value->return_type)
            || value->has_body > 1u) return 0;
        for (child = 0u; child < value->parameter_count; ++child) {
            if (!cm_decl_type_local(metadata,
                    value->parameter_types[child])) return 0;
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

static int cm_decl_validate_predicates(
    const CmHirDeclarationMetadata *metadata)
{
    size_t index;
    size_t total_arguments;
    if (metadata->predicate_count != 0u && metadata->predicates == NULL)
        return 0;
    total_arguments = 0u;
    for (index = 0u; index < metadata->predicate_count; ++index) {
        const CmHirDeclarationPredicate *predicate;
        const CmHirDeclarationTrait *trait_value;
        const CmHirDeclarationType *subject;
        uint32_t child;
        predicate = &metadata->predicates[index];
        if (predicate->owner_value == 0u
            || (size_t)predicate->owner_value > metadata->value_count
            || predicate->trait_local == 0u
            || (size_t)predicate->trait_local > metadata->trait_count
            || !cm_decl_type_local(metadata, predicate->subject_type))
            return 0;
        trait_value = &metadata->traits[predicate->trait_local - 1u];
        subject = &metadata->types[predicate->subject_type - 1u];
        if (subject->kind != CM_HIR_DECL_TYPE_GENERIC
            || !cm_decl_generic_belongs_to_value(metadata,
                subject->generic_local, predicate->owner_value)
            || predicate->argument_count != trait_value->generic_count
            || predicate->argument_count
                > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || !cm_size_add(total_arguments, predicate->argument_count,
                &total_arguments)
            || total_arguments > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || (predicate->argument_count != 0u
                && predicate->argument_types == NULL)) return 0;
        for (child = 0u; child < predicate->argument_count; ++child) {
            const CmHirDeclarationType *argument;
            if (!cm_decl_type_local(metadata,
                    predicate->argument_types[child])) return 0;
            argument = &metadata->types[
                predicate->argument_types[child] - 1u];
            if (argument->kind != CM_HIR_DECL_TYPE_PRIMITIVE) return 0;
        }
        if (index != 0u) {
            const CmHirDeclarationPredicate *prior;
            prior = &metadata->predicates[index - 1u];
            if (prior->owner_value > predicate->owner_value
                || (prior->owner_value == predicate->owner_value
                    && prior->ordinal >= predicate->ordinal)) return 0;
        }
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirDeclarationValue *value;
        uint32_t child;
        value = &metadata->values[index];
        for (child = 0u; child < value->predicate_count; ++child) {
            const CmHirDeclarationPredicate *predicate;
            predicate = &metadata->predicates[
                (size_t)value->predicate_start + child - 1u];
            if (predicate->owner_value != (uint32_t)(index + 1u)
                || predicate->ordinal != child) return 0;
        }
    }
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
        seen[value->return_type - 1u] = 1u;
        for (child = 0u; child < value->parameter_count; ++child)
            seen[value->parameter_types[child] - 1u] = 1u;
    }
    for (index = 0u; index < metadata->predicate_count; ++index) {
        const CmHirDeclarationPredicate *predicate;
        uint32_t child;
        predicate = &metadata->predicates[index];
        seen[predicate->subject_type - 1u] = 1u;
        for (child = 0u; child < predicate->argument_count; ++child)
            seen[predicate->argument_types[child] - 1u] = 1u;
    }
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item = &metadata->items[index];
        if (item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS)
            seen[item->alias_target_type - 1u] = 1u;
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
        }
        scopes[index] = scope;
    }
    for (index = 0u; index < metadata->value_count && valid; ++index) {
        const CmHirDeclarationValue *value = &metadata->values[index];
        uint32_t owner = (uint32_t)(index + 1u);
        uint32_t child;
        uint32_t scope = scopes[value->return_type - 1u];
        if (scope != 0u && scope != owner) valid = 0;
        for (child = 0u; child < value->parameter_count && valid; ++child) {
            scope = scopes[value->parameter_types[child] - 1u];
            if (scope != 0u && scope != owner) valid = 0;
        }
    }
    for (index = 0u; index < metadata->predicate_count && valid; ++index) {
        const CmHirDeclarationPredicate *predicate =
            &metadata->predicates[index];
        uint32_t owner = predicate->owner_value;
        uint32_t child;
        uint32_t scope = scopes[predicate->subject_type - 1u];
        if (scope != 0u && scope != owner) valid = 0;
        for (child = 0u; child < predicate->argument_count && valid;
                ++child) {
            scope = scopes[predicate->argument_types[child] - 1u];
            if (scope != 0u && scope != owner) valid = 0;
        }
    }
    cm_free(scopes);
    return valid;
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
                    && entry->target_kind != CM_HIR_DECL_TARGET_NOMINAL)
                || entry->target_local == 0u
                || (entry->target_kind == CM_HIR_DECL_TARGET_MODULE
                    && (size_t)entry->target_local > metadata->module_count)
                || (entry->target_kind == CM_HIR_DECL_TARGET_ITEM
                    && (size_t)entry->target_local > metadata->item_count)
                || (entry->target_kind == CM_HIR_DECL_TARGET_NOMINAL
                    && (size_t)entry->target_local > metadata->trait_count))
                return 0;
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
                    && entry->target_kind != CM_HIR_DECL_TARGET_VALUE)
                || entry->target_local == 0u
                || (entry->target_kind == CM_HIR_DECL_TARGET_ITEM
                    && ((size_t)entry->target_local > metadata->item_count
                        || metadata->items[entry->target_local - 1u].kind
                            != CM_HIR_DECL_ITEM_STRUCT))
                || (entry->target_kind == CM_HIR_DECL_TARGET_VALUE
                    && (size_t)entry->target_local > metadata->value_count))
                return 0;
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
    for (index = 0u; index < metadata->item_count; ++index) {
        const CmHirDeclarationItem *item;
        size_t entry_index;
        size_t type_defining_count;
        size_t constructor_defining_count;
        item = &metadata->items[index];
        type_defining_count = 0u;
        constructor_defining_count = 0u;
        for (entry_index = 0u; entry_index < metadata->namespace_count;
                ++entry_index) {
            const CmHirDeclarationNamespaceEntry *entry;
            entry = &metadata->namespace_entries[entry_index];
            if (entry->owner_module == item->owner_module
                && entry->target_kind == CM_HIR_DECL_TARGET_ITEM
                && entry->target_local == (uint32_t)(index + 1u)
                && entry->export_ordinal == item->source_ordinal
                && cm_decl_string_equal(entry->name, item->name)) {
                if (entry->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE)
                    type_defining_count += 1u;
                else if (entry->namespace_kind
                        == CM_HIR_DECL_NAMESPACE_VALUE
                    && item->kind == CM_HIR_DECL_ITEM_STRUCT)
                    constructor_defining_count += 1u;
            }
        }
        if (type_defining_count != 1u
            || ((item->kind == CM_HIR_DECL_ITEM_TYPE_ALIAS
                    || item->kind == CM_HIR_DECL_ITEM_ENUM)
                && constructor_defining_count != 0u)
            || (item->kind == CM_HIR_DECL_ITEM_STRUCT
                && constructor_defining_count > 1u)) return 0;
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
                || item->kind == CM_HIR_DECL_ITEM_ENUM) {
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
        int found;
        trait_value = &metadata->traits[index];
        found = 0;
        for (entry_index = 0u; entry_index < metadata->namespace_count;
                ++entry_index) {
            const CmHirDeclarationNamespaceEntry *entry;
            entry = &metadata->namespace_entries[entry_index];
            if (entry->owner_module == trait_value->owner_module
                && entry->namespace_kind == CM_HIR_DECL_NAMESPACE_TYPE
                && entry->target_kind == CM_HIR_DECL_TARGET_NOMINAL
                && entry->target_local == (uint32_t)(index + 1u)
                && cm_decl_string_equal(entry->name, trait_value->name)) {
                found = 1;
            }
        }
        if (!found) return 0;
    }
    for (index = 0u; index < metadata->value_count; ++index) {
        const CmHirDeclarationValue *value;
        size_t entry_index;
        int found;
        value = &metadata->values[index];
        found = 0;
        for (entry_index = 0u; entry_index < metadata->namespace_count;
                ++entry_index) {
            const CmHirDeclarationNamespaceEntry *entry;
            entry = &metadata->namespace_entries[entry_index];
            if (entry->owner_module == value->owner_module
                && entry->namespace_kind == CM_HIR_DECL_NAMESPACE_VALUE
                && entry->target_kind == CM_HIR_DECL_TARGET_VALUE
                && entry->target_local == (uint32_t)(index + 1u)
                && cm_decl_string_equal(entry->name, value->name)) {
                found = 1;
            }
        }
        if (!found) return 0;
    }
    return 1;
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
        || !cm_decl_validate_values(metadata)
        || !cm_decl_validate_generics(metadata)
        || !cm_decl_validate_types(metadata)
        || !cm_decl_validate_predicates(metadata)
        || !cm_decl_validate_value_type_scopes(metadata)
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
        trait_value = &metadata->traits[index];
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(2));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, trait_value->owner_module);
        cm_decl_write_string(&writer, trait_value->name);
        cm_decl_write_visibility(&writer);
        cm_hir_metadata_write_u32(&writer, trait_value->source_ordinal);
        cm_hir_metadata_write_u32(&writer, trait_value->generic_start);
        cm_hir_metadata_write_u32(&writer, trait_value->generic_count);
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    }

    cm_hir_metadata_writer_init(&writer, &sections[4],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, UINT32_C(0));

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
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    }

    cm_hir_metadata_writer_init(&writer, &sections[6],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->type_count);
    for (index = 0u; index < metadata->type_count; ++index) {
        const CmHirDeclarationType *type;
        type = &metadata->types[index];
        cm_hir_metadata_write_u8(&writer, type->kind);
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
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
        } else {
            uint32_t child;
            cm_hir_metadata_write_u32(&writer, type->item_local);
            cm_hir_metadata_write_u32(&writer, type->argument_count);
            for (child = 0u; child < type->argument_count; ++child)
                cm_hir_metadata_write_u32(&writer,
                    type->argument_types[child]);
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
        /* The three predicate ranges are empty in this slice. */
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        if (item->kind == CM_HIR_DECL_ITEM_STRUCT) {
            /* STRUCT payload: UNIT form, no fields. */
            cm_hir_metadata_write_u8(&writer, UINT8_C(1));
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
            cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        } else if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t child;
            cm_hir_metadata_write_u8(&writer,
                item->enum_repr_primitive);
            cm_hir_metadata_write_u8(&writer, UINT8_C(0));
            cm_hir_metadata_write_u16(&writer, UINT16_C(0));
            cm_hir_metadata_write_u32(&writer, item->variant_count);
            for (child = 0u; child < item->variant_count; ++child) {
                const CmHirDeclarationVariant *variant =
                    &item->variants[child];
                cm_hir_metadata_write_u8(&writer, variant->kind);
                cm_hir_metadata_write_u8(&writer,
                    variant->discriminant_primitive);
                cm_hir_metadata_write_u16(&writer, UINT16_C(0));
                cm_decl_write_string(&writer, variant->name);
                cm_hir_metadata_write_u32(&writer,
                    variant->source_ordinal);
                cm_hir_metadata_write_u64(&writer,
                    variant->discriminant_low);
                cm_hir_metadata_write_u64(&writer,
                    variant->discriminant_high);
            }
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
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
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
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
    }

    cm_hir_metadata_writer_init(&writer, &sections[9],
        (size_t)CM_HIR_METADATA_MAX_PAYLOAD_SIZE);
    cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    cm_hir_metadata_write_u32(&writer, (uint32_t)metadata->predicate_count);
    cm_hir_metadata_write_u32(&writer, UINT32_C(0));
    for (index = 0u; index < metadata->predicate_count; ++index) {
        const CmHirDeclarationPredicate *predicate;
        uint32_t child;
        predicate = &metadata->predicates[index];
        cm_hir_metadata_write_u8(&writer, UINT8_C(4));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
        cm_hir_metadata_write_u32(&writer, predicate->owner_value);
        cm_hir_metadata_write_u32(&writer, predicate->ordinal);
        cm_hir_metadata_write_u32(&writer, predicate->subject_type);
        cm_hir_metadata_write_u32(&writer, predicate->trait_local);
        cm_hir_metadata_write_u32(&writer, predicate->argument_count);
        for (child = 0u; child < predicate->argument_count; ++child)
            cm_hir_metadata_write_u32(&writer,
                predicate->argument_types[child]);
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u32(&writer, UINT32_C(0));
        cm_hir_metadata_write_u8(&writer, UINT8_C(1));
        cm_hir_metadata_write_u8(&writer, UINT8_C(0));
        cm_hir_metadata_write_u16(&writer, UINT16_C(0));
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
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader, CM_HIR_DECL_METADATA_MAX_NOMINALS,
            &metadata->trait_count)) return 0;
    metadata->traits = metadata->trait_count == 0u ? NULL
        : (CmHirDeclarationTrait *)cm_alloc_zeroed(metadata->trait_count,
            sizeof(CmHirDeclarationTrait));
    for (index = 0u; index < metadata->trait_count; ++index) {
        CmHirDeclarationTrait *trait_value;
        trait_value = &metadata->traits[index];
        if (!cm_decl_read_u8(&reader, UINT8_C(1))
            || !cm_decl_read_u8(&reader, UINT8_C(2))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &trait_value->owner_module)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_string(&reader, &trait_value->name)
            || !cm_decl_read_visibility(&reader)
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->source_ordinal) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->generic_start) != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u32(&reader,
                &trait_value->generic_count) != CM_HIR_METADATA_OK
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))) return 0;
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
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
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
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))) return 0;
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
    size_t total_variants;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_count(&reader, CM_HIR_DECL_METADATA_MAX_ITEMS,
            &metadata->item_count)) return 0;
    metadata->items = metadata->item_count == 0u ? NULL
        : (CmHirDeclarationItem *)cm_alloc_zeroed(metadata->item_count,
            sizeof(CmHirDeclarationItem));
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
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))) return 0;
        if (item->kind == CM_HIR_DECL_ITEM_STRUCT) {
            if (!cm_decl_read_u8(&reader, UINT8_C(1))
                || !cm_decl_read_u8(&reader, UINT8_C(0))
                || !cm_decl_read_u16(&reader, UINT16_C(0))
                || !cm_decl_read_u32(&reader, UINT32_C(0))) return 0;
        } else if (item->kind == CM_HIR_DECL_ITEM_ENUM) {
            uint32_t child;
            if (cm_hir_metadata_read_u8(&reader,
                    &item->enum_repr_primitive) != CM_HIR_METADATA_OK
                || !cm_decl_read_u8(&reader, UINT8_C(0))
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
                if (cm_hir_metadata_read_u8(&reader, &variant->kind)
                        != CM_HIR_METADATA_OK
                    || cm_hir_metadata_read_u8(&reader,
                        &variant->discriminant_primitive)
                        != CM_HIR_METADATA_OK
                    || !cm_decl_read_u16(&reader, UINT16_C(0))
                    || !cm_decl_read_string(&reader, &variant->name)
                    || cm_hir_metadata_read_u32(&reader,
                        &variant->source_ordinal) != CM_HIR_METADATA_OK
                    || cm_hir_metadata_read_u64(&reader,
                        &variant->discriminant_low) != CM_HIR_METADATA_OK
                    || cm_hir_metadata_read_u64(&reader,
                        &variant->discriminant_high) != CM_HIR_METADATA_OK)
                    return 0;
            }
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
        value = &metadata->values[index];
        if (!cm_decl_read_u8(&reader, UINT8_C(1))
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
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || cm_hir_metadata_read_u32(&reader, &value->parameter_count)
                != CM_HIR_METADATA_OK
            || value->parameter_count
                > (uint32_t)CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES
            || !cm_size_add(total_parameters, value->parameter_count,
                &total_parameters)
            || total_parameters > CM_HIR_DECL_METADATA_MAX_GRAPH_EDGES)
            return 0;
        value->parameter_types = value->parameter_count == 0u ? NULL
            : (uint32_t *)cm_alloc((size_t)value->parameter_count
                * sizeof(uint32_t));
        for (child = 0u; child < value->parameter_count; ++child) {
            if (cm_hir_metadata_read_u32(&reader,
                    &value->parameter_types[child]) != CM_HIR_METADATA_OK)
                return 0;
        }
        if (cm_hir_metadata_read_u32(&reader, &value->return_type)
                != CM_HIR_METADATA_OK
            || cm_hir_metadata_read_u8(&reader, &value->has_body)
                != CM_HIR_METADATA_OK
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))) return 0;
    }
    return cm_decl_reader_done(&reader);
}

static int cm_decl_parse_predicates(const CmHirMetadataSection *section,
    CmHirDeclarationMetadata *metadata)
{
    CmHirMetadataReader reader;
    size_t count;
    size_t index;
    size_t total_arguments;
    cm_hir_metadata_reader_init(&reader, section->data, section->length);
    if (!cm_decl_read_u32(&reader, UINT32_C(0))
        || !cm_decl_read_count(&reader,
            CM_HIR_DECL_METADATA_MAX_PREDICATES, &count)
        || !cm_decl_read_u32(&reader, UINT32_C(0))) return 0;
    metadata->predicate_count = count;
    metadata->predicates = count == 0u ? NULL
        : (CmHirDeclarationPredicate *)cm_alloc_zeroed(count,
            sizeof(CmHirDeclarationPredicate));
    total_arguments = 0u;
    for (index = 0u; index < count; ++index) {
        CmHirDeclarationPredicate *predicate;
        uint32_t child;
        predicate = &metadata->predicates[index];
        if (!cm_decl_read_u8(&reader, UINT8_C(4))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))
            || cm_hir_metadata_read_u32(&reader, &predicate->owner_value)
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
        predicate->argument_types = predicate->argument_count == 0u ? NULL
            : (uint32_t *)cm_alloc((size_t)predicate->argument_count
                * sizeof(uint32_t));
        for (child = 0u; child < predicate->argument_count; ++child) {
            if (cm_hir_metadata_read_u32(&reader,
                    &predicate->argument_types[child])
                    != CM_HIR_METADATA_OK) return 0;
        }
        if (!cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u32(&reader, UINT32_C(0))
            || !cm_decl_read_u8(&reader, UINT8_C(1))
            || !cm_decl_read_u8(&reader, UINT8_C(0))
            || !cm_decl_read_u16(&reader, UINT16_C(0))) return 0;
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
        || !cm_decl_parse_zero(&sections[4])
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
