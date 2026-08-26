#include "cm/hir/declaration_materialize.h"
#include "cm/hir/lower.h"

#include <assert.h>
#include <string.h>

#define S(text) { (unsigned char *)(text), sizeof(text) - 1u }

typedef struct TestFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationString cfgs[2];
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationGeneric generics[2];
    CmHirDeclarationType types[3];
    CmHirDeclarationItem items[1];
    CmHirDeclarationValue values[1];
    uint32_t parameters[1];
    CmHirDeclarationPredicate predicates[1];
    uint32_t predicate_arguments[1];
    CmHirDeclarationNamespaceEntry namespace_entries[8];
} TestFixture;

typedef struct AliasFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[1];
    CmHirDeclarationItem items[3];
    CmHirDeclarationNamespaceEntry namespace_entries[6];
} AliasFixture;

typedef struct CompositeFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationTrait traits[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationGeneric generics[3];
    CmHirDeclarationType types[7];
    uint32_t application_arguments[1];
    CmHirDeclarationValue values[1];
    uint32_t parameters[4];
    CmHirDeclarationPredicate predicates[1];
    uint32_t predicate_arguments[1];
    CmHirDeclarationNamespaceEntry namespace_entries[4];
} CompositeFixture;

typedef struct EnumFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationItem items[1];
    CmHirDeclarationVariant variants[2];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} EnumFixture;

typedef struct DefaultEnumFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationItem items[2];
    CmHirDeclarationVariant variants[4];
    CmHirDeclarationNamespaceEntry namespace_entries[6];
} DefaultEnumFixture;

typedef struct ConstFixture {
    CmHirDeclarationMetadata metadata;
    CmHirDeclarationModule modules[1];
    CmHirDeclarationType types[1];
    CmHirDeclarationValue values[1];
    CmHirDeclarationNamespaceEntry namespace_entries[2];
} ConstFixture;

typedef struct ContextLengths {
    size_t crates;
    size_t modules;
    size_t items;
    size_t bodies;
    size_t expressions;
    size_t types;
    size_t generics;
    size_t definitions;
    size_t strings;
} ContextLengths;

static void fixture_init(TestFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator = (CmHirDeclarationString)S("gate-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->cfgs[0] = (CmHirDeclarationString)S("target_arch=x86_64");
    fixture->cfgs[1] =
        (CmHirDeclarationString)S("target_pointer_width=64");
    metadata->cfgs = fixture->cfgs;
    metadata->cfg_count = 2u;

    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].name = (CmHirDeclarationString)S("Gate");
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].generic_start = 1u;
    fixture->traits[0].generic_count = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].index = 0u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].is_relaxed_sized = 1u;
    fixture->generics[0].name = (CmHirDeclarationString)S("T");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[1].owner_local = 1u;
    fixture->generics[1].index = 0u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("X");
    metadata->generics = fixture->generics;
    metadata->generic_count = 2u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 2u;
    metadata->types = fixture->types;
    metadata->type_count = 3u;

    fixture->parameters[0] = 3u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("needs");
    fixture->values[0].source_ordinal = 2u;
    fixture->values[0].generic_start = 2u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].parameter_count = 1u;
    fixture->values[0].parameter_types = fixture->parameters;
    fixture->values[0].return_type = 1u;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->predicate_arguments[0] = 2u;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].ordinal = 0u;
    fixture->predicates[0].subject_type = 3u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].argument_count = 1u;
    fixture->predicates[0].argument_types = fixture->predicate_arguments;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1].owner_module = 1u;
    fixture->namespace_entries[1].namespace_kind = CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("GateReexport");
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[1].target_local = 1u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2].owner_module = 1u;
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[2].name = fixture->values[0].name;
    fixture->namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[2].target_local = 1u;
    fixture->namespace_entries[2].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 3u;
}

static void item_fixture_init(TestFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    fixture_init(fixture);
    metadata = &fixture->metadata;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Packet");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 3u;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->namespace_entries[2].owner_module = 1u;
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[2].name = fixture->items[0].name;
    fixture->namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[2].target_local = 1u;
    fixture->namespace_entries[2].export_ordinal = 3u;
    fixture->namespace_entries[3] = fixture->namespace_entries[2];
    fixture->namespace_entries[3].name =
        (CmHirDeclarationString)S("PacketReexport");
    fixture->namespace_entries[3].export_ordinal = 4u;
    fixture->namespace_entries[4] = fixture->namespace_entries[2];
    fixture->namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[5] = fixture->namespace_entries[3];
    fixture->namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[6].owner_module = 1u;
    fixture->namespace_entries[6].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[6].name = fixture->values[0].name;
    fixture->namespace_entries[6].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[6].target_local = 1u;
    fixture->namespace_entries[6].export_ordinal = 2u;
    metadata->namespace_count = 7u;
}

static void alias_fixture_init(AliasFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("layout-error-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("AllocError");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 1u;
    fixture->items[1].kind = CM_HIR_DECL_ITEM_TYPE_ALIAS;
    fixture->items[1].owner_module = 1u;
    fixture->items[1].name = (CmHirDeclarationString)S("LayoutErr");
    fixture->items[1].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[1].source_ordinal = 2u;
    fixture->items[1].alias_target_type = 1u;
    fixture->items[2].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[2].owner_module = 1u;
    fixture->items[2].name = (CmHirDeclarationString)S("LayoutError");
    fixture->items[2].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[2].source_ordinal = 3u;
    metadata->items = fixture->items;
    metadata->item_count = 3u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_NAMED_ADT;
    fixture->types[0].item_local = 3u;
    metadata->types = fixture->types;
    metadata->type_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->items[1].name;
    fixture->namespace_entries[1].target_local = 2u;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2] = fixture->namespace_entries[1];
    fixture->namespace_entries[2].name =
        (CmHirDeclarationString)S("LayoutErrReexport");
    fixture->namespace_entries[2].export_ordinal = 4u;
    fixture->namespace_entries[3] = fixture->namespace_entries[0];
    fixture->namespace_entries[3].name = fixture->items[2].name;
    fixture->namespace_entries[3].target_local = 3u;
    fixture->namespace_entries[3].export_ordinal = 3u;
    fixture->namespace_entries[4] = fixture->namespace_entries[3];
    fixture->namespace_entries[4].name =
        (CmHirDeclarationString)S("LayoutErrorReexport");
    fixture->namespace_entries[4].export_ordinal = 5u;
    fixture->namespace_entries[5] = fixture->namespace_entries[0];
    fixture->namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 6u;
}

static void composite_fixture_init(CompositeFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;
    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("composite-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->traits[0].owner_module = 1u;
    fixture->traits[0].name = (CmHirDeclarationString)S("Gate");
    fixture->traits[0].source_ordinal = 1u;
    fixture->traits[0].generic_start = 1u;
    fixture->traits[0].generic_count = 1u;
    metadata->traits = fixture->traits;
    metadata->trait_count = 1u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_STRUCT;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Wrap");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 2u;
    fixture->items[0].generic_start = 2u;
    fixture->items[0].generic_count = 1u;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->generics[0].owner_kind = CM_HIR_DECL_GENERIC_NOMINAL;
    fixture->generics[0].owner_local = 1u;
    fixture->generics[0].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[0].name = (CmHirDeclarationString)S("G");
    fixture->generics[1].owner_kind = CM_HIR_DECL_GENERIC_ITEM;
    fixture->generics[1].owner_local = 1u;
    fixture->generics[1].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[1].name = (CmHirDeclarationString)S("T");
    fixture->generics[2].owner_kind = CM_HIR_DECL_GENERIC_VALUE;
    fixture->generics[2].owner_local = 1u;
    fixture->generics[2].kind = CM_HIR_DECL_GENERIC_TYPE;
    fixture->generics[2].name = (CmHirDeclarationString)S("X");
    metadata->generics = fixture->generics;
    metadata->generic_count = 3u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_UNIT;
    fixture->types[1].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[1].primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->types[2].kind = CM_HIR_DECL_TYPE_GENERIC;
    fixture->types[2].generic_local = 3u;
    fixture->types[3].kind = CM_HIR_DECL_TYPE_SLICE;
    fixture->types[3].child_type = 2u;
    fixture->types[4].kind = CM_HIR_DECL_TYPE_RAW_POINTER;
    fixture->types[4].child_type = 2u;
    fixture->types[4].mutability = CM_HIR_DECL_MUTABLE;
    fixture->types[5].kind = CM_HIR_DECL_TYPE_REFERENCE;
    fixture->types[5].child_type = 2u;
    fixture->types[5].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->types[5].region.kind = CM_HIR_DECL_REGION_STATIC;
    fixture->application_arguments[0] = 2u;
    fixture->types[6].kind = CM_HIR_DECL_TYPE_NAMED_ADT_APPLICATION;
    fixture->types[6].item_local = 1u;
    fixture->types[6].argument_count = 1u;
    fixture->types[6].argument_types = fixture->application_arguments;
    metadata->types = fixture->types;
    metadata->type_count = 7u;

    fixture->parameters[0] = 4u;
    fixture->parameters[1] = 5u;
    fixture->parameters[2] = 6u;
    fixture->parameters[3] = 7u;
    fixture->values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("inspect");
    fixture->values[0].source_ordinal = 3u;
    fixture->values[0].generic_start = 3u;
    fixture->values[0].generic_count = 1u;
    fixture->values[0].predicate_start = 1u;
    fixture->values[0].predicate_count = 1u;
    fixture->values[0].parameter_count = 4u;
    fixture->values[0].parameter_types = fixture->parameters;
    fixture->values[0].return_type = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->predicate_arguments[0] = 2u;
    fixture->predicates[0].owner_value = 1u;
    fixture->predicates[0].subject_type = 3u;
    fixture->predicates[0].trait_local = 1u;
    fixture->predicates[0].argument_count = 1u;
    fixture->predicates[0].argument_types = fixture->predicate_arguments;
    metadata->predicates = fixture->predicates;
    metadata->predicate_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->traits[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name = fixture->items[0].name;
    fixture->namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[1].export_ordinal = 2u;
    fixture->namespace_entries[2] = fixture->namespace_entries[1];
    fixture->namespace_entries[2].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[3].owner_module = 1u;
    fixture->namespace_entries[3].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[3].name = fixture->values[0].name;
    fixture->namespace_entries[3].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[3].target_local = 1u;
    fixture->namespace_entries[3].export_ordinal = 3u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 4u;
}

static void enum_fixture_init(EnumFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("char-enum-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->variants[0].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[0].name = (CmHirDeclarationString)S("Null");
    fixture->variants[0].source_ordinal = 2u;
    fixture->variants[0].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;
    fixture->variants[0].discriminant_low = 0u;
    fixture->variants[1].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[1].name = (CmHirDeclarationString)S("Scalar");
    fixture->variants[1].source_ordinal = 3u;
    fixture->variants[1].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;
    fixture->variants[1].discriminant_low = 255u;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("Char");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    /* Canonical item and defining export ordinals may begin at zero. */
    fixture->items[0].source_ordinal = 0u;
    fixture->items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U8;
    fixture->items[0].variant_count = 2u;
    fixture->items[0].variants = fixture->variants;
    metadata->items = fixture->items;
    metadata->item_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 0u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("CharReexport");
    fixture->namespace_entries[1].export_ordinal = 1u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static void default_enum_fixture_init(DefaultEnumFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("mir-reason-enum-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->variants[0].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[0].name = (CmHirDeclarationString)S("Normal");
    fixture->variants[0].source_ordinal = 1u;
    fixture->variants[0].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;
    fixture->variants[1].kind = CM_HIR_DECL_VARIANT_UNIT;
    fixture->variants[1].name = (CmHirDeclarationString)S("Cleanup");
    fixture->variants[1].source_ordinal = 2u;
    fixture->variants[1].discriminant_primitive =
        CM_HIR_DECL_VARIANT_DISCRIMINANT_IMPLICIT;

    fixture->items[0].kind = CM_HIR_DECL_ITEM_ENUM;
    fixture->items[0].owner_module = 1u;
    fixture->items[0].name = (CmHirDeclarationString)S("BasicBlock");
    fixture->items[0].visibility.kind = CM_HIR_DECL_VISIBILITY_PUBLIC;
    fixture->items[0].source_ordinal = 0u;
    fixture->items[0].enum_repr_primitive = CM_HIR_DECL_ENUM_REPR_RUST;
    fixture->items[0].variant_count = 2u;
    fixture->items[0].variants = fixture->variants;
    fixture->items[0].diagnostic_item =
        (CmHirDeclarationString)S("mir_basic_block");
    fixture->items[1] = fixture->items[0];
    fixture->items[1].name =
        (CmHirDeclarationString)S("UnwindTerminateReason");
    fixture->items[1].source_ordinal = 3u;
    fixture->items[1].variants = &fixture->variants[2];
    fixture->items[1].diagnostic_item =
        (CmHirDeclarationString)S("mir_unwind_terminate_reason");
    fixture->variants[2] = fixture->variants[0];
    fixture->variants[2].name = (CmHirDeclarationString)S("Abi");
    fixture->variants[2].source_ordinal = 4u;
    fixture->variants[3] = fixture->variants[1];
    fixture->variants[3].name = (CmHirDeclarationString)S("InCleanup");
    fixture->variants[3].source_ordinal = 5u;
    metadata->items = fixture->items;
    metadata->item_count = 2u;

    /* Canonical NSPC order is TYPE names followed by VALUE names. */
    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    fixture->namespace_entries[0].name = fixture->items[0].name;
    fixture->namespace_entries[0].target_kind =
        CM_HIR_DECL_TARGET_ITEM;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 0u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("ReasonAbi");
    fixture->namespace_entries[1].target_kind =
        CM_HIR_DECL_TARGET_ENUM_VARIANT;
    fixture->namespace_entries[1].target_local = 3u;
    fixture->namespace_entries[1].export_ordinal = 6u;
    fixture->namespace_entries[2] = fixture->namespace_entries[0];
    fixture->namespace_entries[2].name =
        (CmHirDeclarationString)S("ReasonInCleanup");
    fixture->namespace_entries[2].target_kind =
        CM_HIR_DECL_TARGET_ENUM_VARIANT;
    fixture->namespace_entries[2].target_local = 4u;
    fixture->namespace_entries[2].export_ordinal = 6u;
    fixture->namespace_entries[3] = fixture->namespace_entries[0];
    fixture->namespace_entries[3].name = fixture->items[1].name;
    fixture->namespace_entries[3].target_local = 2u;
    fixture->namespace_entries[3].export_ordinal = 3u;
    fixture->namespace_entries[4] = fixture->namespace_entries[1];
    fixture->namespace_entries[4].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[5] = fixture->namespace_entries[2];
    fixture->namespace_entries[5].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 6u;
}

static void const_fixture_init(ConstFixture *fixture)
{
    CmHirDeclarationMetadata *metadata;

    memset(fixture, 0, sizeof(*fixture));
    metadata = &fixture->metadata;
    metadata->crate_name = (CmHirDeclarationString)S("depcrate");
    metadata->crate_disambiguator =
        (CmHirDeclarationString)S("char-const-v1");
    metadata->edition = CM_HIR_DECL_EDITION_2021;
    metadata->target_triple =
        (CmHirDeclarationString)S("x86_64-unknown-linux-gnu");
    metadata->data_layout = (CmHirDeclarationString)S("e-p:64:64");
    metadata->panic_strategy = CM_HIR_DECL_PANIC_ABORT;
    fixture->modules[0].name = metadata->crate_name;
    metadata->root_module = 1u;
    metadata->modules = fixture->modules;
    metadata->module_count = 1u;

    fixture->types[0].kind = CM_HIR_DECL_TYPE_PRIMITIVE;
    fixture->types[0].primitive = CM_HIR_DECL_PRIMITIVE_CHAR;
    metadata->types = fixture->types;
    metadata->type_count = 1u;

    fixture->values[0].kind = CM_HIR_DECL_VALUE_CONST;
    fixture->values[0].owner_module = 1u;
    fixture->values[0].name = (CmHirDeclarationString)S("MAX");
    fixture->values[0].source_ordinal = 1u;
    fixture->values[0].declared_type = 1u;
    fixture->values[0].mutability = CM_HIR_DECL_IMMUTABLE;
    fixture->values[0].has_body = 1u;
    metadata->values = fixture->values;
    metadata->value_count = 1u;

    fixture->namespace_entries[0].owner_module = 1u;
    fixture->namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    fixture->namespace_entries[0].name = fixture->values[0].name;
    fixture->namespace_entries[0].target_kind = CM_HIR_DECL_TARGET_VALUE;
    fixture->namespace_entries[0].target_local = 1u;
    fixture->namespace_entries[0].export_ordinal = 1u;
    fixture->namespace_entries[1] = fixture->namespace_entries[0];
    fixture->namespace_entries[1].name =
        (CmHirDeclarationString)S("MAX_REEXPORT");
    fixture->namespace_entries[1].export_ordinal = 2u;
    metadata->namespace_entries = fixture->namespace_entries;
    metadata->namespace_count = 2u;
}

static CmHirDeclarationMaterializeExpectation expectation_for(
    const CmHirDeclarationMetadata *metadata)
{
    CmHirDeclarationMaterializeExpectation expectation;
    memset(&expectation, 0, sizeof(expectation));
    expectation.crate_name = metadata->crate_name;
    expectation.crate_disambiguator = metadata->crate_disambiguator;
    expectation.edition = metadata->edition;
    expectation.target_triple = metadata->target_triple;
    expectation.data_layout = metadata->data_layout;
    expectation.panic_strategy = metadata->panic_strategy;
    expectation.cfgs = metadata->cfgs;
    expectation.cfg_count = metadata->cfg_count;
    return expectation;
}

static ContextLengths context_lengths(const CmHirContext *context)
{
    ContextLengths lengths;
    lengths.crates = context->crates.len;
    lengths.modules = context->modules.len;
    lengths.items = context->items.len;
    lengths.bodies = context->bodies.len;
    lengths.expressions = context->expressions.len;
    lengths.types = context->types.len;
    lengths.generics = context->generic_parameters.len;
    lengths.definitions = context->definitions.len;
    lengths.strings = cm_interner_length(&context->strings);
    return lengths;
}

static void assert_context_lengths(const CmHirContext *context,
    ContextLengths expected)
{
    ContextLengths actual = context_lengths(context);
    assert(actual.crates == expected.crates);
    assert(actual.modules == expected.modules);
    assert(actual.items == expected.items);
    assert(actual.bodies == expected.bodies);
    assert(actual.expressions == expected.expressions);
    assert(actual.types == expected.types);
    assert(actual.generics == expected.generics);
    assert(actual.definitions == expected.definitions);
    assert(actual.strings == expected.strings);
}

static CmHirLibraryBinding lookup_binding(const CmHirLibraryArtifact *artifact,
    const char *name)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    return binding;
}

static CmHirLibraryBinding lookup_value_binding(
    const CmHirLibraryArtifact *artifact, const char *name)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_value_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    return binding;
}

static CmHirLibraryStatus lookup_value_binding_status(
    const CmHirLibraryArtifact *artifact, const char *name)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryBinding binding;
    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&binding, 0, sizeof(binding));
    return cm_hir_library_artifact_lookup_value_binding(artifact, path, 2u,
        &binding);
}

static const CmHirItem *find_item(const CmHirContext *context,
    CmHirItemKind kind, const char *name)
{
    size_t index;
    size_t length = strlen(name);
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item = (const CmHirItem *)cm_vec_at_const(
            &context->items, index);
        const CmInternedString *item_name = item == NULL ? NULL
            : cm_interner_get(&context->strings, item->name);
        if (item != NULL && item->kind == kind && item_name != NULL
            && item_name->len == length
            && memcmp(item_name->bytes, name, length) == 0) return item;
    }
    return NULL;
}

static void assert_gate_predicate(const CmHirContext *context,
    const CmHirItem *item, CmHirDefId gate)
{
    const CmHirTraitPredicate *predicate;
    const CmHirType *subject;
    const CmHirType *argument;
    const CmHirGenericParam *parameter;
    assert(item != NULL && item->kind == CM_HIR_ITEM_FUNCTION
        && item->generic_parameter_count == 1u
        && item->predicate_count == 1u);
    predicate = &item->predicates[0];
    assert(cm_hir_def_id_equal(predicate->trait_type.definition, gate)
        && predicate->trait_type.argument_count == 1u
        && predicate->trait_type.arguments[0].kind == CM_HIR_GENERIC_ARG_TYPE);
    subject = cm_hir_get_type(context, predicate->subject);
    argument = cm_hir_get_type(context,
        predicate->trait_type.arguments[0].data.type);
    parameter = cm_hir_get_generic_param(context,
        item->generic_parameter_start);
    assert(subject != NULL && subject->kind == CM_HIR_TYPE_PARAMETER_KIND
        && subject->data.parameter_type.parameter
            == item->generic_parameter_start
        && parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && cm_hir_def_id_equal(parameter->owner, item->definition)
        && argument != NULL && argument->kind == CM_HIR_TYPE_INTEGER_KIND
        && argument->data.integer_type.kind == CM_HIR_INT_U8);
}

static void test_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId gate)
{
    static const unsigned char source_text[] =
        "use dep::GateReexport;\n"
        "pub fn direct<X: dep::Gate<u8>>(_x: X) {}\n"
        "pub fn via_alias<X: GateReexport<u8>>(_x: X) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *direct;
    const CmHirItem *via_alias;
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    direct = find_item(context, CM_HIR_ITEM_FUNCTION, "direct");
    via_alias = find_item(context, CM_HIR_ITEM_FUNCTION, "via_alias");
    assert_gate_predicate(context, direct, gate);
    assert_gate_predicate(context, via_alias, gate);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_const_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId constant)
{
    static const unsigned char source_text[] =
        "use dep::MAX;\n"
        "use dep::MAX_REEXPORT;\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirModule *root;
    uint32_t import_index;
    uint32_t matched;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "const_consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "const_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    root = cm_hir_get_module(context, lower_result.root_module);
    assert(root != NULL && root->import_count == 2u);
    matched = 0u;
    for (import_index = 0u; import_index < root->import_count; ++import_index) {
        const CmHirImport *import_value = &root->imports[import_index];
        const CmHirImportBinding *binding;
        const CmInternedString *name;
        assert(import_value->binding_count == 1u
            && import_value->bindings != NULL);
        binding = &import_value->bindings[0];
        name = cm_interner_get(&context->strings, binding->name);
        assert(binding->namespace_kind == CM_HIR_NAMESPACE_VALUE
            && cm_hir_def_id_equal(binding->target, constant)
            && name != NULL
            && ((name->len == sizeof("MAX") - 1u
                    && memcmp(name->bytes, "MAX", name->len) == 0)
                || (name->len == sizeof("MAX_REEXPORT") - 1u
                    && memcmp(name->bytes, "MAX_REEXPORT", name->len)
                        == 0)));
        matched += 1u;
    }
    assert(matched == 2u);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void assert_item_parameter(const CmHirContext *context,
    const CmHirItem *function, CmHirDefId item_definition)
{
    const CmHirType *parameter;
    assert(function != NULL && function->kind == CM_HIR_ITEM_FUNCTION
        && function->data.function_item.signature.parameter_count == 1u);
    parameter = cm_hir_get_type(context,
        function->data.function_item.signature.parameters[0].type);
    assert(parameter != NULL && parameter->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(parameter->data.named_type.definition,
            item_definition)
        && parameter->data.named_type.argument_count == 0u);
}

static void test_item_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId item_definition)
{
    static const unsigned char source_text[] =
        "pub fn direct_item(_x: dep::Packet) {}\n"
        "pub fn reexported_item(_x: dep::PacketReexport) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirItem *direct;
    const CmHirItem *reexported;
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "item_consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "item_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    direct = find_item(context, CM_HIR_ITEM_FUNCTION, "direct_item");
    reexported = find_item(context, CM_HIR_ITEM_FUNCTION,
        "reexported_item");
    assert_item_parameter(context, direct, item_definition);
    assert_item_parameter(context, reexported, item_definition);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_alias_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId layout_error)
{
    static const unsigned char source_text[] =
        "pub fn direct(_x: dep::LayoutError) {}\n"
        "pub fn direct_reexport(_x: dep::LayoutErrorReexport) {}\n"
        "pub fn alias(_x: dep::LayoutErr) {}\n"
        "pub fn alias_reexport(_x: dep::LayoutErrReexport) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "alias_consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "alias_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    assert_item_parameter(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "direct"), layout_error);
    assert_item_parameter(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "direct_reexport"),
        layout_error);
    assert_item_parameter(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "alias"), layout_error);
    assert_item_parameter(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "alias_reexport"),
        layout_error);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void assert_u8_type(const CmHirContext *context, CmHirTypeId id)
{
    const CmHirType *type = cm_hir_get_type(context, id);
    assert(type != NULL && type->kind == CM_HIR_TYPE_INTEGER_KIND
        && type->data.integer_type.kind == CM_HIR_INT_U8);
}

static void assert_composite_signature(const CmHirContext *context,
    const CmHirItem *function, CmHirDefId wrap)
{
    const CmHirFunctionSignature *signature;
    const CmHirType *type;
    assert(function != NULL && function->kind == CM_HIR_ITEM_FUNCTION);
    signature = &function->data.function_item.signature;
    assert(signature->parameter_count == 4u);
    type = cm_hir_get_type(context, signature->parameters[0].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_SLICE_KIND);
    assert_u8_type(context, type->data.slice_type.element);
    type = cm_hir_get_type(context, signature->parameters[1].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_RAW_POINTER_KIND
        && type->data.raw_pointer_type.mutability == CM_HIR_MUTABLE);
    assert_u8_type(context, type->data.raw_pointer_type.pointee);
    type = cm_hir_get_type(context, signature->parameters[2].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
        && type->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && type->data.reference_type.region.kind == CM_HIR_REGION_STATIC);
    assert_u8_type(context, type->data.reference_type.pointee);
    type = cm_hir_get_type(context, signature->parameters[3].type);
    assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type->data.named_type.definition, wrap)
        && type->data.named_type.argument_count == 1u
        && type->data.named_type.arguments != NULL
        && type->data.named_type.arguments[0].kind
            == CM_HIR_GENERIC_ARG_TYPE);
    assert_u8_type(context, type->data.named_type.arguments[0].data.type);
}

static void assert_imported_composite_signature(const CmHirContext *context,
    const CmHirItem *function, CmHirDefId wrap)
{
    const CmHirType *reference;
    const CmHirType *slice;
    assert(function != NULL && function->kind == CM_HIR_ITEM_FUNCTION
        && function->data.function_item.signature.parameter_count == 4u);
    reference = cm_hir_get_type(context,
        function->data.function_item.signature.parameters[0].type);
    assert(reference != NULL
        && reference->kind == CM_HIR_TYPE_REFERENCE_KIND
        && reference->data.reference_type.mutability == CM_HIR_IMMUTABLE
        && reference->data.reference_type.region.kind == CM_HIR_REGION_STATIC);
    slice = cm_hir_get_type(context, reference->data.reference_type.pointee);
    assert(slice != NULL && slice->kind == CM_HIR_TYPE_SLICE_KIND);
    assert_u8_type(context, slice->data.slice_type.element);
    {
        const CmHirFunctionSignature *signature =
            &function->data.function_item.signature;
        const CmHirType *type = cm_hir_get_type(context,
            signature->parameters[1].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_RAW_POINTER_KIND
            && type->data.raw_pointer_type.mutability == CM_HIR_MUTABLE);
        assert_u8_type(context, type->data.raw_pointer_type.pointee);
        type = cm_hir_get_type(context, signature->parameters[2].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_REFERENCE_KIND
            && type->data.reference_type.mutability == CM_HIR_IMMUTABLE
            && type->data.reference_type.region.kind == CM_HIR_REGION_STATIC);
        assert_u8_type(context, type->data.reference_type.pointee);
        type = cm_hir_get_type(context, signature->parameters[3].type);
        assert(type != NULL && type->kind == CM_HIR_TYPE_ADT_KIND
            && cm_hir_def_id_equal(type->data.named_type.definition, wrap)
            && type->data.named_type.argument_count == 1u
            && type->data.named_type.arguments[0].kind
                == CM_HIR_GENERIC_ARG_TYPE);
        assert_u8_type(context,
            type->data.named_type.arguments[0].data.type);
    }
}

static void test_composite_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId wrap)
{
    static const unsigned char source_text[] =
        "pub fn imported(_a: &'static [u8], _b: *mut u8, "
        "_c: &'static u8, _d: dep::Wrap<u8>) {}\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "composite_consumer.rs",
        source_text, sizeof(source_text) - 1u, &root_source)
        == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "composite_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    assert(lower_result.error_count == 0u);
    assert_imported_composite_signature(context,
        find_item(context, CM_HIR_ITEM_FUNCTION, "imported"), wrap);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void assert_artifact_identity_same(const CmHirLibraryArtifact *artifact,
    const CmHirLibraryArtifactIdentity *expected)
{
    CmHirLibraryArtifactIdentity actual;
    assert(cm_hir_library_artifact_identity(artifact, &actual));
    assert(actual.context == expected->context
        && actual.crate_id == expected->crate_id
        && cm_hir_def_id_equal(actual.root_definition,
            expected->root_definition)
        && strcmp(actual.extern_name, expected->extern_name) == 0);
}

static void assert_expectation_rejected(CmHirContext *context,
    CmHirLibraryArtifact *artifact, const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationMaterializeExpectation *expectation,
    ContextLengths lengths, const CmHirLibraryArtifactIdentity *identity)
{
    CmHirDeclarationMaterializeResult result;
    result = cm_hir_declaration_metadata_materialize(context, artifact,
        metadata, expectation, "replacement", 91u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA
        && result.metadata_status
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    assert_context_lengths(context, lengths);
    assert_artifact_identity_same(artifact, identity);
}

static void assert_item_metadata_rejected(CmHirContext *context,
    CmHirLibraryArtifact *artifact, const CmHirDeclarationMetadata *metadata,
    const CmHirDeclarationMaterializeExpectation *expectation,
    ContextLengths lengths, const CmHirLibraryArtifactIdentity *identity,
    CmSourceId source)
{
    CmHirDeclarationMaterializeResult result;
    result = cm_hir_declaration_metadata_materialize(context, artifact,
        metadata, expectation, "replacement", source);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA
        && result.metadata_status
            == CM_HIR_DECL_METADATA_UNSUPPORTED_DESCRIPTOR);
    assert_context_lengths(context, lengths);
    assert_artifact_identity_same(artifact, identity);
}

static void test_materialize_decode_and_consume(void)
{
    TestFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryBinding canonical;
    CmHirLibraryBinding reexport;
    CmHirLibraryValue needs;
    CmHirLibraryPathSegment value_path[2];
    const CmHirItem *gate_item;
    const CmHirItem *needs_item;
    const CmHirGenericParam *trait_parameter;
    ContextLengths lengths;
    CmHirLibraryArtifactIdentity identity;
    CmHirDeclarationMaterializeExpectation wrong;
    CmHirDeclarationString saved_string;
    uint8_t saved_byte;
    size_t saved_count;
    uint32_t saved_owner;

    fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 77u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u
        && result.item_count == 0u
        && result.public_type_entry_count == 2u
        && result.public_value_entry_count == 1u);
    canonical = lookup_binding(&artifact, "Gate");
    reexport = lookup_binding(&artifact, "GateReexport");
    assert(canonical.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && reexport.kind == CM_HIR_LIBRARY_BINDING_TRAIT
        && cm_hir_def_id_equal(canonical.definition, reexport.definition));
    gate_item = find_item(&context, CM_HIR_ITEM_TRAIT, "Gate");
    needs_item = find_item(&context, CM_HIR_ITEM_FUNCTION, "needs");
    assert(gate_item != NULL && gate_item->generic_parameter_count == 1u
        && cm_hir_def_id_equal(gate_item->definition, canonical.definition));
    trait_parameter = cm_hir_get_generic_param(&context,
        gate_item->generic_parameter_start);
    assert(trait_parameter != NULL && trait_parameter->is_relaxed_sized == 1
        && cm_hir_def_id_equal(trait_parameter->owner,
            gate_item->definition));
    assert_gate_predicate(&context, needs_item, canonical.definition);
    value_path[0].bytes = (const unsigned char *)"dep";
    value_path[0].length = 3u;
    value_path[1].bytes = (const unsigned char *)"needs";
    value_path[1].length = 5u;
    memset(&needs, 0, sizeof(needs));
    assert(cm_hir_library_artifact_lookup_value(&artifact, value_path, 2u,
        &needs) == CM_HIR_LIBRARY_OK
        && needs.data.function.predicate_count == 1u
        && needs.data.function.nominal_reference_count == 1u
        && cm_hir_def_id_equal(needs.data.function.nominal_references[0]
                .definition,
            canonical.definition));

    test_fresh_consumer(&context, &artifact, canonical.definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    wrong = expectation;
    wrong.crate_name = (CmHirDeclarationString)S("wrongcrate");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.crate_disambiguator = (CmHirDeclarationString)S("wrong-disamb");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.edition = CM_HIR_DECL_EDITION_2024;
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.target_triple = (CmHirDeclarationString)S("aarch64-unknown-linux-gnu");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.data_layout = (CmHirDeclarationString)S("e-p:32:32");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.panic_strategy = CM_HIR_DECL_PANIC_UNWIND;
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    wrong.cfg_count = 1u;
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);
    wrong = expectation;
    {
        CmHirDeclarationString wrong_cfgs[2];
        wrong_cfgs[0] = (CmHirDeclarationString)S("target_arch=aarch64");
        wrong_cfgs[1] = decoded.cfgs[1];
        wrong.cfgs = wrong_cfgs;
        assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
            lengths, &identity);
    }

    saved_owner = decoded.predicates[0].owner_value;
    decoded.predicates[0].owner_value = 2u;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 92u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.predicates[0].owner_value = saved_owner;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    saved_string = decoded.modules[0].name;
    decoded.modules[0].name = (CmHirDeclarationString)S("wrongroot");
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 93u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.modules[0].name = saved_string;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    saved_byte = decoded.namespace_entries[1].target_kind;
    decoded.namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_VALUE;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 94u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.namespace_entries[1].target_kind = saved_byte;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    saved_count = decoded.predicates[0].argument_count;
    decoded.predicates[0].argument_count = 0u;
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "replacement", 95u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_INVALID_METADATA);
    decoded.predicates[0].argument_count = (uint32_t)saved_count;
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 96u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_item_materialize_and_consume(void)
{
    TestFixture fixture;
    CmByteBuf encoded;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeExpectation wrong;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding type_binding;
    CmHirLibraryBinding type_reexport;
    CmHirLibraryBinding constructor;
    CmHirLibraryBinding constructor_reexport;
    CmHirLibraryPathSegment path[2];
    CmHirLibraryType type;
    CmHirLibraryValue value;
    const CmHirItem *item;
    const CmHirDefinition *definition;
    ContextLengths lengths;
    uint8_t saved_kind;
    uint32_t saved_local;

    item_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 101u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u
        && result.item_count == 1u
        && result.public_type_entry_count == 4u
        && result.public_value_entry_count == 3u);

    type_binding = lookup_binding(&artifact, "Packet");
    type_reexport = lookup_binding(&artifact, "PacketReexport");
    constructor = lookup_value_binding(&artifact, "Packet");
    constructor_reexport = lookup_value_binding(&artifact,
        "PacketReexport");
    assert(type_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && type_binding.type_kind == CM_HIR_TYPE_ADT_KIND
        && type_reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && constructor.kind
            == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
        && constructor.value_kind == CM_HIR_LIBRARY_VALUE_NONE
        && constructor_reexport.kind
            == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
        && cm_hir_def_id_equal(type_binding.definition,
            type_reexport.definition)
        && cm_hir_def_id_equal(type_binding.definition,
            constructor.definition)
        && cm_hir_def_id_equal(type_binding.definition,
            constructor_reexport.definition));
    item = find_item(&context, CM_HIR_ITEM_STRUCT, "Packet");
    definition = item == NULL ? NULL
        : cm_hir_lookup_definition(&context, item->definition);
    assert(item != NULL
        && definition != NULL
        && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->kind == CM_HIR_DEFINITION_ITEM
        && definition->has_reserved_item_kind
        && definition->reserved_item_kind == CM_HIR_ITEM_STRUCT
        && cm_hir_def_id_equal(item->definition, type_binding.definition)
        && item->owner_module == result.root_module
        && item->visibility.kind == CM_HIR_VIS_PUBLIC
        && item->generic_parameter_count == 0u
        && item->predicate_scope_count == 0u
        && item->predicate_count == 0u
        && item->outlives_predicate_count == 0u
        && item->attribute_count == 0u
        && item->data.aggregate_item.form == CM_HIR_AGGREGATE_UNIT
        && item->data.aggregate_item.field_count == 0u
        && item->data.aggregate_item.fields == NULL);

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = 3u;
    path[1].bytes = (const unsigned char *)"Packet";
    path[1].length = 6u;
    memset(&type, 0, sizeof(type));
    assert(cm_hir_library_artifact_lookup_type(&artifact, path, 2u, &type)
        == CM_HIR_LIBRARY_OK
        && type.kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type.definition, item->definition));
    memset(&value, 0, sizeof(value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u, &value)
        == CM_HIR_LIBRARY_WRONG_NAMESPACE);

    test_item_fresh_consumer(&context, &artifact, item->definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    wrong = expectation;
    wrong.crate_disambiguator =
        (CmHirDeclarationString)S("wrong-item-disambiguator");
    assert_expectation_rejected(&context, &artifact, &decoded, &wrong,
        lengths, &identity);

    saved_kind = decoded.namespace_entries[2].target_kind;
    decoded.namespace_entries[2].target_kind = CM_HIR_DECL_TARGET_NOMINAL;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 102u);
    decoded.namespace_entries[2].target_kind = saved_kind;

    saved_kind = decoded.namespace_entries[4].target_kind;
    decoded.namespace_entries[4].target_kind = CM_HIR_DECL_TARGET_VALUE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 103u);
    decoded.namespace_entries[4].target_kind = saved_kind;

    saved_local = decoded.namespace_entries[4].target_local;
    decoded.namespace_entries[4].target_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 104u);
    decoded.namespace_entries[4].target_local = saved_local;

    saved_kind = decoded.items[0].kind;
    decoded.items[0].kind = UINT8_C(1);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 105u);
    decoded.items[0].kind = saved_kind;

    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "bad-name", 106u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_ARTIFACT_FAILURE
        && result.library_status == CM_HIR_LIBRARY_INVALID_ARGUMENT);
    assert_context_lengths(&context, lengths);
    assert_artifact_identity_same(&artifact, &identity);

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&encoded);
}

static void test_alias_materialize_and_consume(void)
{
    AliasFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding alloc_error;
    CmHirLibraryBinding alias;
    CmHirLibraryBinding alias_reexport;
    CmHirLibraryBinding layout_error;
    CmHirLibraryBinding layout_error_reexport;
    const CmHirItem *alias_item;
    const CmHirItem *layout_error_item;
    const CmHirDefinition *alias_definition;
    const CmHirDefinition *layout_error_definition;
    const CmHirType *alias_target;
    ContextLengths lengths;
    uint8_t saved_kind;
    uint32_t saved_local;

    alias_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 111u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u
        && result.item_count == 3u
        && result.public_type_entry_count == 5u
        && result.public_value_entry_count == 1u);

    alloc_error = lookup_value_binding(&artifact, "AllocError");
    alias = lookup_binding(&artifact, "LayoutErr");
    alias_reexport = lookup_binding(&artifact, "LayoutErrReexport");
    layout_error = lookup_binding(&artifact, "LayoutError");
    layout_error_reexport = lookup_binding(&artifact,
        "LayoutErrorReexport");
    assert(alloc_error.kind == CM_HIR_LIBRARY_BINDING_STRUCT_CONSTRUCTOR
        && alias.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alias.type_kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND
        && alias_reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && alias_reexport.type_kind == CM_HIR_TYPE_ALIAS_APPLICATION_KIND
        && layout_error.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && layout_error.type_kind == CM_HIR_TYPE_ADT_KIND
        && layout_error_reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && layout_error_reexport.type_kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(alias.definition,
            alias_reexport.definition)
        && cm_hir_def_id_equal(layout_error.definition,
            layout_error_reexport.definition)
        && !cm_hir_def_id_equal(alias.definition, layout_error.definition));
    assert(lookup_value_binding_status(&artifact, "LayoutErr")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "LayoutErrReexport")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "LayoutError")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "LayoutErrorReexport")
            == CM_HIR_LIBRARY_NOT_FOUND);

    alias_item = find_item(&context, CM_HIR_ITEM_TYPE_ALIAS, "LayoutErr");
    layout_error_item = find_item(&context, CM_HIR_ITEM_STRUCT,
        "LayoutError");
    alias_definition = alias_item == NULL ? NULL
        : cm_hir_lookup_definition(&context, alias_item->definition);
    layout_error_definition = layout_error_item == NULL ? NULL
        : cm_hir_lookup_definition(&context, layout_error_item->definition);
    alias_target = alias_item == NULL ? NULL : cm_hir_get_type(&context,
        alias_item->data.type_alias_item.target);
    assert(alias_item != NULL && layout_error_item != NULL
        && alias_definition != NULL
        && alias_definition->state == CM_HIR_DEFINITION_BOUND
        && alias_definition->kind == CM_HIR_DEFINITION_ITEM
        && alias_definition->has_reserved_item_kind
        && alias_definition->reserved_item_kind == CM_HIR_ITEM_TYPE_ALIAS
        && layout_error_definition != NULL
        && layout_error_definition->state == CM_HIR_DEFINITION_BOUND
        && layout_error_definition->kind == CM_HIR_DEFINITION_ITEM
        && layout_error_definition->has_reserved_item_kind
        && layout_error_definition->reserved_item_kind == CM_HIR_ITEM_STRUCT
        && cm_hir_def_id_equal(alias_item->definition, alias.definition)
        && cm_hir_def_id_equal(layout_error_item->definition,
            layout_error.definition)
        && cm_hir_def_id_is_none(alias_item->parent_definition)
        && cm_hir_def_id_is_none(
            alias_item->data.type_alias_item.trait_item_definition)
        && alias_item->data.type_alias_item.bound_count == 0u
        && alias_item->data.type_alias_item.bounds == NULL
        && alias_target != NULL && alias_target->kind == CM_HIR_TYPE_ADT_KIND
        && alias_target->data.named_type.argument_count == 0u
        && cm_hir_def_id_equal(alias_target->data.named_type.definition,
            layout_error.definition)
        && layout_error_item->data.aggregate_item.form
            == CM_HIR_AGGREGATE_UNIT);

    test_alias_fresh_consumer(&context, &artifact,
        layout_error.definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_local = decoded.items[1].alias_target_type;
    decoded.items[1].alias_target_type = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 112u);
    decoded.items[1].alias_target_type = saved_local;

    saved_local = decoded.types[0].item_local;
    decoded.types[0].item_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 113u);
    decoded.types[0].item_local = saved_local;

    saved_local = decoded.namespace_entries[5].target_local;
    decoded.namespace_entries[5].target_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 114u);
    decoded.namespace_entries[5].target_local = saved_local;

    saved_kind = decoded.items[1].kind;
    decoded.items[1].kind = CM_HIR_DECL_ITEM_STRUCT;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 115u);
    decoded.items[1].kind = saved_kind;

    saved_local = decoded.namespace_entries[1].target_local;
    decoded.namespace_entries[1].target_local = 3u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 116u);
    decoded.namespace_entries[1].target_local = saved_local;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_composite_materialize_and_consume(void)
{
    CompositeFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding wrap_binding;
    const CmHirItem *wrap;
    const CmHirItem *inspect;
    const CmHirGenericParam *parameter;
    ContextLengths lengths;
    uint8_t saved_kind;
    uint32_t saved_count;
    uint32_t saved_local;

    composite_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 121u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.item_count == 1u
        && result.public_type_entry_count == 2u
        && result.public_value_entry_count == 2u);
    wrap_binding = lookup_binding(&artifact, "Wrap");
    wrap = find_item(&context, CM_HIR_ITEM_STRUCT, "Wrap");
    inspect = find_item(&context, CM_HIR_ITEM_FUNCTION, "inspect");
    assert(wrap_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && wrap_binding.type_kind == CM_HIR_TYPE_ADT_KIND
        && wrap != NULL
        && cm_hir_def_id_equal(wrap_binding.definition, wrap->definition)
        && wrap->generic_parameter_count == 1u
        && wrap->generic_parameter_start != CM_HIR_GENERIC_PARAM_NONE
        && wrap->data.aggregate_item.form == CM_HIR_AGGREGATE_UNIT);
    parameter = cm_hir_get_generic_param(&context,
        wrap->generic_parameter_start);
    assert(parameter != NULL && parameter->kind == CM_HIR_GENERIC_TYPE
        && parameter->index == 0u
        && cm_hir_def_id_equal(parameter->owner, wrap->definition));
    assert_composite_signature(&context, inspect, wrap->definition);
    test_composite_fresh_consumer(&context, &artifact, wrap->definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_kind = decoded.types[5].region.kind;
    decoded.types[5].region.kind = CM_HIR_DECL_REGION_EARLY_BOUND;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 122u);
    decoded.types[5].region.kind = saved_kind;

    saved_count = decoded.types[6].argument_count;
    decoded.types[6].argument_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 123u);
    decoded.types[6].argument_count = saved_count;

    saved_local = decoded.generics[1].owner_local;
    decoded.generics[1].owner_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 124u);
    decoded.generics[1].owner_local = saved_local;

    saved_kind = decoded.types[3].kind;
    decoded.types[3].kind = CM_HIR_DECL_TYPE_SELF;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 125u);
    decoded.types[3].kind = saved_kind;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_const_materialize_and_consume(void)
{
    ConstFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding reexport;
    CmHirLibraryValue direct_value;
    CmHirLibraryValue reexport_value;
    CmHirLibraryPathSegment path[2];
    const CmHirItem *constant;
    const CmHirDefinition *definition;
    const CmHirType *declared_type;
    ContextLengths lengths;
    uint32_t saved_local;
    uint8_t saved_byte;

    const_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 141u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u && result.item_count == 0u
        && result.public_type_entry_count == 0u
        && result.public_value_entry_count == 2u
        && context.bodies.len == 0u);
    direct = lookup_value_binding(&artifact, "MAX");
    reexport = lookup_value_binding(&artifact, "MAX_REEXPORT");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && direct.value_kind == CM_HIR_LIBRARY_VALUE_CONST
        && reexport.kind == CM_HIR_LIBRARY_BINDING_VALUE
        && reexport.value_kind == CM_HIR_LIBRARY_VALUE_CONST
        && cm_hir_def_id_equal(direct.definition, reexport.definition));

    constant = find_item(&context, CM_HIR_ITEM_CONST, "MAX");
    definition = constant == NULL ? NULL
        : cm_hir_lookup_definition(&context, constant->definition);
    declared_type = constant == NULL ? NULL
        : cm_hir_get_type(&context, constant->data.value_item.type);
    assert(constant != NULL && definition != NULL
        && definition->kind == CM_HIR_DEFINITION_ITEM
        && definition->state == CM_HIR_DEFINITION_BOUND
        && definition->has_reserved_item_kind
        && definition->reserved_item_kind == CM_HIR_ITEM_CONST
        && cm_hir_def_id_equal(constant->definition, direct.definition)
        && cm_hir_def_id_is_none(constant->parent_definition)
        && constant->generic_parameter_count == 0u
        && constant->predicate_count == 0u
        && constant->data.value_item.body == CM_HIR_BODY_NONE
        && constant->data.value_item.definition_kind
            == CM_HIR_VALUE_DEFINITION_METADATA_DECLARATION
        && constant->data.value_item.has_default_body == 0
        && constant->data.value_item.mutability == CM_HIR_IMMUTABLE
        && cm_hir_def_id_is_none(
            constant->data.value_item.trait_item_definition)
        && declared_type != NULL
        && declared_type->kind == CM_HIR_TYPE_CHAR_KIND);

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)"MAX";
    path[1].length = sizeof("MAX") - 1u;
    memset(&direct_value, 0, sizeof(direct_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
        &direct_value) == CM_HIR_LIBRARY_OK);
    path[1].bytes = (const unsigned char *)"MAX_REEXPORT";
    path[1].length = sizeof("MAX_REEXPORT") - 1u;
    memset(&reexport_value, 0, sizeof(reexport_value));
    assert(cm_hir_library_artifact_lookup_value(&artifact, path, 2u,
        &reexport_value) == CM_HIR_LIBRARY_OK
        && direct_value.kind == CM_HIR_LIBRARY_VALUE_CONST
        && reexport_value.kind == CM_HIR_LIBRARY_VALUE_CONST
        && cm_hir_def_id_equal(direct_value.definition,
            reexport_value.definition)
        && direct_value.data.value.type == constant->data.value_item.type
        && reexport_value.data.value.type == constant->data.value_item.type
        && direct_value.data.value.mutability == CM_HIR_IMMUTABLE
        && reexport_value.data.value.mutability == CM_HIR_IMMUTABLE);

    test_const_fresh_consumer(&context, &artifact, constant->definition);
    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_byte = decoded.values[0].kind;
    decoded.values[0].kind = CM_HIR_DECL_VALUE_FUNCTION;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 142u);
    decoded.values[0].kind = saved_byte;

    saved_local = decoded.values[0].declared_type;
    decoded.values[0].declared_type = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 143u);
    decoded.values[0].declared_type = saved_local;

    saved_byte = decoded.values[0].mutability;
    decoded.values[0].mutability = CM_HIR_DECL_MUTABLE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 144u);
    decoded.values[0].mutability = saved_byte;

    saved_byte = decoded.values[0].has_body;
    decoded.values[0].has_body = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 145u);
    decoded.values[0].has_body = saved_byte;

    saved_local = decoded.values[0].return_type;
    decoded.values[0].return_type = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 146u);
    decoded.values[0].return_type = saved_local;

    saved_local = decoded.namespace_entries[1].target_local;
    decoded.namespace_entries[1].target_local = 2u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 147u);
    decoded.namespace_entries[1].target_local = saved_local;

    saved_byte = decoded.namespace_entries[0].namespace_kind;
    decoded.namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_TYPE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 148u);
    decoded.namespace_entries[0].namespace_kind = saved_byte;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void assert_enum_variant_path(const CmHirLibraryArtifact *artifact,
    const char *enum_name, const char *variant_name,
    CmHirDefId enum_definition, CmHirDefId variant_definition,
    uint32_t variant_index)
{
    CmHirLibraryPathSegment path[3];
    CmHirLibraryType type;
    CmHirLibraryBinding binding;
    CmHirLibraryValue value;

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)enum_name;
    path[1].length = strlen(enum_name);
    path[2].bytes = (const unsigned char *)variant_name;
    path[2].length = strlen(variant_name);
    memset(&type, 0, sizeof(type));
    assert(cm_hir_library_artifact_lookup_type(artifact, path, 3u, &type)
        == CM_HIR_LIBRARY_OK);
    assert(type.binding_kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && type.kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type.definition, variant_definition)
        && cm_hir_def_id_equal(type.enum_definition, enum_definition)
        && type.enum_variant_index == variant_index
        && type.enum_variant_form == CM_HIR_AGGREGATE_UNIT);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_value_binding(artifact, path, 3u,
        &binding) == CM_HIR_LIBRARY_OK);
    assert(binding.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && cm_hir_def_id_equal(binding.definition, variant_definition)
        && cm_hir_def_id_equal(binding.enum_definition, enum_definition)
        && binding.enum_variant_index == variant_index
        && binding.enum_variant_form == CM_HIR_AGGREGATE_UNIT);
    memset(&value, 0, sizeof(value));
    assert(cm_hir_library_artifact_lookup_value(artifact, path, 3u, &value)
        == CM_HIR_LIBRARY_WRONG_NAMESPACE);
}

static void assert_enum_variant_alias(const CmHirLibraryArtifact *artifact,
    const char *name, CmHirDefId enum_definition,
    CmHirDefId variant_definition, uint32_t variant_index)
{
    CmHirLibraryPathSegment path[2];
    CmHirLibraryType type;
    CmHirLibraryBinding binding;
    CmHirLibraryValue value;

    path[0].bytes = (const unsigned char *)"dep";
    path[0].length = sizeof("dep") - 1u;
    path[1].bytes = (const unsigned char *)name;
    path[1].length = strlen(name);
    memset(&type, 0, sizeof(type));
    assert(cm_hir_library_artifact_lookup_type(artifact, path, 2u, &type)
        == CM_HIR_LIBRARY_OK);
    assert(type.binding_kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && type.kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(type.definition, variant_definition)
        && cm_hir_def_id_equal(type.enum_definition, enum_definition)
        && type.enum_variant_index == variant_index
        && type.enum_variant_form == CM_HIR_AGGREGATE_UNIT);
    memset(&binding, 0, sizeof(binding));
    assert(cm_hir_library_artifact_lookup_value_binding(artifact, path, 2u,
        &binding) == CM_HIR_LIBRARY_OK);
    assert(binding.kind == CM_HIR_LIBRARY_BINDING_ENUM_VARIANT
        && cm_hir_def_id_equal(binding.definition, variant_definition)
        && cm_hir_def_id_equal(binding.enum_definition, enum_definition)
        && binding.enum_variant_index == variant_index
        && binding.enum_variant_form == CM_HIR_AGGREGATE_UNIT
        && binding.enum_variant_namespace
            == CM_HIR_LIBRARY_ENUM_VARIANT_VALUE);
    memset(&value, 0, sizeof(value));
    assert(cm_hir_library_artifact_lookup_value(artifact, path, 2u, &value)
        == CM_HIR_LIBRARY_WRONG_NAMESPACE);
}

static void test_default_enum_fresh_consumer(CmHirContext *context,
    const CmHirLibraryArtifact *artifact, CmHirDefId abi,
    CmHirDefId cleanup)
{
    static const unsigned char source_text[] =
        "use dep::UnwindTerminateReason::Abi;\n"
        "use dep::ReasonAbi;\n"
        "use dep::ReasonInCleanup;\n";
    CmSourceSet sources;
    CmSourceId root_source;
    CmModuleGraph graph;
    CmCfgSet cfg;
    CmModuleGraphOptions graph_options;
    CmModuleGraphResult graph_result;
    CmImportResolver imports;
    CmImportResult import_result;
    CmHirModuleMap map;
    CmHirLowerOptions lower_options;
    CmHirLowerResult lower_result;
    const CmHirLibraryArtifact *libraries[1];
    const CmHirModule *root;
    uint32_t import_index;
    uint32_t type_count;
    uint32_t value_count;

    cm_source_set_init(&sources);
    cm_module_graph_init(&graph);
    cm_cfg_set_init(&cfg);
    cm_import_resolver_init(&imports);
    cm_hir_module_map_init(&map);
    assert(cm_source_add_memory(&sources, "variant_consumer.rs", source_text,
        sizeof(source_text) - 1u, &root_source) == CM_SOURCE_OK);
    cm_module_graph_options_init(&graph_options);
    graph_options.cfg = &cfg;
    graph_result = cm_module_graph_build(&graph, &sources, root_source,
        &graph_options);
    assert(graph_result.error_count == 0u);
    import_result = cm_import_resolve(&imports, &graph,
        graph_result.revision);
    assert(import_result.revision == graph_result.revision);
    cm_hir_lower_options_init(&lower_options);
    lower_options.crate_name = "variant_consumer";
    libraries[0] = artifact;
    lower_options.dependency_libraries = libraries;
    lower_options.dependency_library_count = 1u;
    lower_result = cm_hir_lower_module_graph(context, &graph,
        graph_result.revision, &imports, &map, &lower_options);
    if (lower_result.error_count != 0u) {
        fprintf(stderr, "default enum consumer: %s: %s\n",
            cm_hir_lower_error_kind_name(lower_result.first_error.kind),
            lower_result.first_error.message);
    }
    assert(lower_result.error_count == 0u);
    root = cm_hir_get_module(context, lower_result.root_module);
    assert(root != NULL && root->import_count == 3u);
    type_count = 0u;
    value_count = 0u;
    for (import_index = 0u; import_index < root->import_count;
            ++import_index) {
        const CmHirImport *import_value = &root->imports[import_index];
        uint32_t binding_index;

        assert(import_value->binding_count == 2u
            && import_value->bindings != NULL);
        for (binding_index = 0u;
                binding_index < import_value->binding_count;
                ++binding_index) {
            const CmHirImportBinding *binding =
                &import_value->bindings[binding_index];
            const CmInternedString *name = cm_interner_get(
                &context->strings, binding->name);
            int is_abi;

            assert(name != NULL);
            is_abi = (name->len == sizeof("Abi") - 1u
                    && memcmp(name->bytes, "Abi", name->len) == 0)
                || (name->len == sizeof("ReasonAbi") - 1u
                    && memcmp(name->bytes, "ReasonAbi", name->len) == 0);
            assert(is_abi
                ? cm_hir_def_id_equal(binding->target, abi)
                : (name->len == sizeof("ReasonInCleanup") - 1u
                    && memcmp(name->bytes, "ReasonInCleanup", name->len)
                        == 0
                    && cm_hir_def_id_equal(binding->target, cleanup)));
            if (binding->namespace_kind == CM_HIR_NAMESPACE_TYPE)
                type_count += 1u;
            else {
                assert(binding->namespace_kind == CM_HIR_NAMESPACE_VALUE);
                value_count += 1u;
            }
        }
    }
    assert(type_count == 3u && value_count == 3u);
    cm_hir_module_map_destroy(&map);
    cm_import_resolver_destroy(&imports);
    cm_module_graph_destroy(&graph);
    cm_source_set_destroy(&sources);
}

static void test_enum_materialize_and_restore_scope(void)
{
    EnumFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding direct;
    CmHirLibraryBinding reexport;
    const CmHirItem *enumeration;
    const CmHirDefinition *enum_definition;
    const CmInternedString *attribute_text;
    ContextLengths lengths;
    uint64_t saved_low;
    uint64_t saved_high;
    uint32_t saved_count;
    uint32_t saved_ordinal;
    uint8_t saved_kind;
    uint8_t saved_primitive;
    uint8_t saved_namespace;
    uint32_t index;

    enum_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK);
    assert(encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 131u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u && result.item_count == 1u
        && result.public_type_entry_count == 2u
        && result.public_value_entry_count == 0u);
    direct = lookup_binding(&artifact, "Char");
    reexport = lookup_binding(&artifact, "CharReexport");
    assert(direct.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && direct.type_kind == CM_HIR_TYPE_ADT_KIND
        && reexport.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && reexport.type_kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(direct.definition, reexport.definition)
        && lookup_value_binding_status(&artifact, "Char")
            == CM_HIR_LIBRARY_NOT_FOUND
        && lookup_value_binding_status(&artifact, "CharReexport")
            == CM_HIR_LIBRARY_NOT_FOUND);

    enumeration = find_item(&context, CM_HIR_ITEM_ENUM, "Char");
    enum_definition = enumeration == NULL ? NULL
        : cm_hir_lookup_definition(&context, enumeration->definition);
    attribute_text = enumeration == NULL || enumeration->attribute_count != 1u
        ? NULL : cm_interner_get(&context.strings,
            enumeration->attributes[0].metadata);
    assert(enumeration != NULL && enum_definition != NULL
        && enum_definition->state == CM_HIR_DEFINITION_BOUND
        && enum_definition->kind == CM_HIR_DEFINITION_ITEM
        && enum_definition->has_reserved_item_kind
        && enum_definition->reserved_item_kind == CM_HIR_ITEM_ENUM
        && cm_hir_def_id_equal(enumeration->definition, direct.definition)
        && enumeration->generic_parameter_count == 0u
        && enumeration->predicate_count == 0u
        && enumeration->attribute_count == 1u
        && enumeration->attributes != NULL && attribute_text != NULL
        && attribute_text->len == sizeof("repr(u8)") - 1u
        && memcmp(attribute_text->bytes, "repr(u8)",
            sizeof("repr(u8)") - 1u) == 0
        && enumeration->attributes[0].span.source == 131u
        && enumeration->attributes[0].span.start == 0u
        && enumeration->attributes[0].span.end == 1u
        && enumeration->attributes[0].source_attribute == 1u
        && enumeration->attributes[0].expansion_depth == 0u
        && enumeration->data.enum_item.variant_count == 2u
        && enumeration->data.enum_item.variants != NULL);
    for (index = 0u; index < 2u; ++index) {
        const CmHirVariant *variant =
            &enumeration->data.enum_item.variants[index];
        const CmHirDefinition *variant_definition =
            cm_hir_lookup_definition(&context, variant->definition);
        const CmHirType *discriminant = cm_hir_get_type(&context,
            variant->discriminant.type);
        const CmInternedString *name = cm_interner_get(&context.strings,
            variant->name);
        assert(variant_definition != NULL
            && variant_definition->kind == CM_HIR_DEFINITION_ENUM_VARIANT
            && variant_definition->state == CM_HIR_DEFINITION_BOUND
            && variant_definition->entity.enum_variant.enum_item_id
                == enum_definition->entity.item_id
            && variant_definition->entity.enum_variant.variant_index == index
            && variant->form == CM_HIR_AGGREGATE_UNIT
            && variant->field_count == 0u && variant->fields == NULL
            && variant->has_discriminant
            && variant->discriminant.kind == CM_HIR_CONST_VALUE
            && variant->discriminant.data.value.low_bits
                == (index == 0u ? 0u : 255u)
            && variant->discriminant.data.value.high_bits == 0u
            && discriminant != NULL
            && discriminant->kind == CM_HIR_TYPE_INTEGER_KIND
            && discriminant->data.integer_type.kind == CM_HIR_INT_ISIZE
            && discriminant->span.source == 131u
            && discriminant->span.start == index + 2u
            && name != NULL);
    }
    assert_enum_variant_path(&artifact, "Char", "Null",
        enumeration->definition,
        enumeration->data.enum_item.variants[0].definition, 0u);
    assert_enum_variant_path(&artifact, "CharReexport", "Null",
        enumeration->definition,
        enumeration->data.enum_item.variants[0].definition, 0u);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_primitive = decoded.items[0].enum_repr_primitive;
    decoded.items[0].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_ISIZE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 132u);
    decoded.items[0].enum_repr_primitive = saved_primitive;

    saved_kind = decoded.items[0].variants[0].kind;
    decoded.items[0].variants[0].kind = UINT8_C(2);
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 133u);
    decoded.items[0].variants[0].kind = saved_kind;

    saved_primitive = decoded.items[0].variants[0].discriminant_primitive;
    decoded.items[0].variants[0].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_U8;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 134u);
    decoded.items[0].variants[0].discriminant_primitive = saved_primitive;

    saved_high = decoded.items[0].variants[0].discriminant_high;
    decoded.items[0].variants[0].discriminant_high = 1u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 135u);
    decoded.items[0].variants[0].discriminant_high = saved_high;

    saved_low = decoded.items[0].variants[1].discriminant_low;
    decoded.items[0].variants[1].discriminant_low = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 136u);
    decoded.items[0].variants[1].discriminant_low = saved_low;

    saved_ordinal = decoded.items[0].variants[1].source_ordinal;
    decoded.items[0].variants[1].source_ordinal =
        decoded.items[0].variants[0].source_ordinal;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 137u);
    decoded.items[0].variants[1].source_ordinal = saved_ordinal;

    saved_count = decoded.items[0].variant_count;
    decoded.items[0].variant_count = 0u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 138u);
    decoded.items[0].variant_count = saved_count;

    saved_namespace = decoded.namespace_entries[0].namespace_kind;
    decoded.namespace_entries[0].namespace_kind =
        CM_HIR_DECL_NAMESPACE_VALUE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 139u);
    decoded.namespace_entries[0].namespace_kind = saved_namespace;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

static void test_default_enum_materialize_and_variant_reexports(void)
{
    DefaultEnumFixture fixture;
    CmByteBuf encoded;
    CmByteBuf replay;
    CmHirDeclarationMetadata decoded;
    CmHirDeclarationMaterializeExpectation expectation;
    CmHirDeclarationMaterializeResult result;
    CmHirContext context;
    CmHirLibraryArtifact artifact;
    CmHirLibraryArtifactIdentity identity;
    CmHirLibraryBinding enum_binding;
    const CmHirItem *basic_block;
    const CmHirItem *enumeration;
    const CmHirDefinition *enum_definition;
    const CmInternedString *attribute_text;
    ContextLengths lengths;
    uint32_t index;
    uint32_t saved_count;
    uint32_t saved_local;
    uint8_t saved_byte;
    CmHirDeclarationString saved_string;

    default_enum_fixture_init(&fixture);
    assert(cm_hir_declaration_metadata_validate(&fixture.metadata)
        == CM_HIR_DECL_METADATA_OK);
    cm_byte_buf_init(&encoded);
    cm_byte_buf_init(&replay);
    assert(cm_hir_declaration_metadata_encode(&fixture.metadata, &encoded)
        == CM_HIR_DECL_METADATA_OK);
    cm_hir_declaration_metadata_init(&decoded);
    assert(cm_hir_declaration_metadata_decode(encoded.data, encoded.len,
        &decoded) == CM_HIR_DECL_METADATA_OK);
    assert(cm_hir_declaration_metadata_encode(&decoded, &replay)
        == CM_HIR_DECL_METADATA_OK
        && encoded.len == replay.len
        && memcmp(encoded.data, replay.data, encoded.len) == 0);

    expectation = expectation_for(&decoded);
    cm_hir_context_init(&context);
    cm_hir_library_artifact_init(&artifact);
    result = cm_hir_declaration_metadata_materialize(&context, &artifact,
        &decoded, &expectation, "dep", 151u);
    assert(result.status == CM_HIR_DECL_MATERIALIZE_OK
        && result.module_count == 1u && result.item_count == 2u
        && result.public_type_entry_count == 4u
        && result.public_value_entry_count == 2u);
    enum_binding = lookup_binding(&artifact, "UnwindTerminateReason");
    basic_block = find_item(&context, CM_HIR_ITEM_ENUM, "BasicBlock");
    enumeration = find_item(&context, CM_HIR_ITEM_ENUM,
        "UnwindTerminateReason");
    enum_definition = enumeration == NULL ? NULL
        : cm_hir_lookup_definition(&context, enumeration->definition);
    attribute_text = enumeration == NULL || enumeration->attribute_count != 1u
        ? NULL : cm_interner_get(&context.strings,
            enumeration->attributes[0].metadata);
    assert(basic_block != NULL && basic_block->attribute_count == 1u
        && basic_block->data.enum_item.variant_count == 2u
        && basic_block->attributes != NULL
        && basic_block->attributes[0].span.source == 151u
        && basic_block->attributes[0].span.start == 0u
        && basic_block->attributes[0].span.end == 1u
        && basic_block->attributes[0].source_attribute == 1u
        && basic_block->attributes[0].expansion_depth == 0u
        && enumeration != NULL && enum_definition != NULL
        && enum_binding.kind == CM_HIR_LIBRARY_BINDING_TYPE
        && enum_binding.type_kind == CM_HIR_TYPE_ADT_KIND
        && cm_hir_def_id_equal(enum_binding.definition,
            enumeration->definition)
        && enumeration->attribute_count == 1u
        && enumeration->attributes != NULL && attribute_text != NULL
        && attribute_text->len
            == sizeof("rustc_diagnostic_item = \"mir_unwind_terminate_reason\"")
                - 1u
        && memcmp(attribute_text->bytes,
            "rustc_diagnostic_item = \"mir_unwind_terminate_reason\"",
            attribute_text->len) == 0
        && enumeration->attributes[0].span.source == 151u
        && enumeration->attributes[0].span.start == 3u
        && enumeration->attributes[0].span.end == 4u
        && enumeration->attributes[0].source_attribute == 1u
        && enumeration->attributes[0].expansion_depth == 0u
        && enumeration->data.enum_item.variant_count == 2u
        && enumeration->data.enum_item.variants != NULL);
    for (index = 0u; index < 2u; ++index) {
        const CmHirVariant *variant =
            &enumeration->data.enum_item.variants[index];
        const CmHirDefinition *definition = cm_hir_lookup_definition(
            &context, variant->definition);
        assert(definition != NULL
            && definition->kind == CM_HIR_DEFINITION_ENUM_VARIANT
            && definition->state == CM_HIR_DEFINITION_BOUND
            && definition->entity.enum_variant.enum_item_id
                == enum_definition->entity.item_id
            && definition->entity.enum_variant.variant_index == index
            && variant->form == CM_HIR_AGGREGATE_UNIT
            && variant->fields == NULL && variant->field_count == 0u
            && !variant->has_discriminant
            && variant->discriminant.kind == CM_HIR_CONST_VALUE
            && variant->discriminant.type == CM_HIR_TYPE_NONE
            && variant->discriminant.data.value.low_bits == 0u
            && variant->discriminant.data.value.high_bits == 0u);
    }
    assert_enum_variant_path(&artifact, "BasicBlock", "Normal",
        basic_block->definition,
        basic_block->data.enum_item.variants[0].definition, 0u);
    assert_enum_variant_path(&artifact, "UnwindTerminateReason", "Abi",
        enumeration->definition,
        enumeration->data.enum_item.variants[0].definition, 0u);
    assert_enum_variant_path(&artifact, "UnwindTerminateReason",
        "InCleanup", enumeration->definition,
        enumeration->data.enum_item.variants[1].definition, 1u);
    assert_enum_variant_alias(&artifact, "ReasonAbi",
        enumeration->definition,
        enumeration->data.enum_item.variants[0].definition, 0u);
    assert_enum_variant_alias(&artifact, "ReasonInCleanup",
        enumeration->definition,
        enumeration->data.enum_item.variants[1].definition, 1u);
    test_default_enum_fresh_consumer(&context, &artifact,
        enumeration->data.enum_item.variants[0].definition,
        enumeration->data.enum_item.variants[1].definition);

    lengths = context_lengths(&context);
    assert(cm_hir_library_artifact_identity(&artifact, &identity));

    saved_string = decoded.items[1].diagnostic_item;
    decoded.items[1].diagnostic_item = (CmHirDeclarationString)S("");
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 152u);
    decoded.items[1].diagnostic_item = saved_string;

    saved_byte = decoded.items[1].enum_repr_primitive;
    decoded.items[1].enum_repr_primitive = CM_HIR_DECL_PRIMITIVE_U8;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 153u);
    decoded.items[1].enum_repr_primitive = saved_byte;

    saved_byte = decoded.items[1].variants[0].discriminant_primitive;
    decoded.items[1].variants[0].discriminant_primitive =
        CM_HIR_DECL_PRIMITIVE_ISIZE;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 154u);
    decoded.items[1].variants[0].discriminant_primitive = saved_byte;

    saved_local = decoded.namespace_entries[1].target_local;
    decoded.namespace_entries[1].target_local = 5u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 155u);
    decoded.namespace_entries[1].target_local = saved_local;

    saved_byte = decoded.namespace_entries[1].target_kind;
    decoded.namespace_entries[1].target_kind = CM_HIR_DECL_TARGET_ITEM;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 156u);
    decoded.namespace_entries[1].target_kind = saved_byte;

    saved_count = (uint32_t)decoded.namespace_count;
    decoded.namespace_count = 5u;
    assert_item_metadata_rejected(&context, &artifact, &decoded,
        &expectation, lengths, &identity, 157u);
    decoded.namespace_count = saved_count;

    cm_hir_library_artifact_destroy(&artifact);
    cm_hir_context_destroy(&context);
    cm_hir_declaration_metadata_destroy(&decoded);
    cm_byte_buf_destroy(&replay);
    cm_byte_buf_destroy(&encoded);
}

int main(void)
{
    test_materialize_decode_and_consume();
    test_item_materialize_and_consume();
    test_alias_materialize_and_consume();
    test_composite_materialize_and_consume();
    test_const_materialize_and_consume();
    test_enum_materialize_and_restore_scope();
    test_default_enum_materialize_and_variant_reexports();
    return 0;
}
