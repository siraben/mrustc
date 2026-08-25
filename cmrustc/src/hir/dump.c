#include "cm/hir/model.h"

static void cm_hir_dump_string(FILE *stream, const CmHirContext *context,
    CmInternId id)
{
    const CmInternedString *string;
    size_t index;

    string = cm_interner_get(&context->strings, id);
    fputc('"', stream);
    if (string != NULL) {
        for (index = 0u; index < string->len; ++index) {
            unsigned char byte;

            byte = string->bytes[index];
            if (byte == (unsigned char)'"' || byte == (unsigned char)'\\') {
                fputc('\\', stream);
                fputc((int)byte, stream);
            } else if (byte == (unsigned char)'\n') {
                fputs("\\n", stream);
            } else if (byte == (unsigned char)'\r') {
                fputs("\\r", stream);
            } else if (byte == (unsigned char)'\t') {
                fputs("\\t", stream);
            } else if (byte >= 0x20u && byte < 0x7fu) {
                fputc((int)byte, stream);
            } else {
                fprintf(stream, "\\x%02x", (unsigned int)byte);
            }
        }
    }
    fputc('"', stream);
}

static void cm_hir_dump_def(FILE *stream, CmHirDefId id)
{
    if (cm_hir_def_id_is_none(id)) {
        fputs("none", stream);
    } else {
        fprintf(stream, "%u:%u", (unsigned int)id.crate_id,
            (unsigned int)id.index);
    }
}

static void cm_hir_dump_span(FILE *stream, CmSpan span)
{
    fprintf(stream, "%u:%u..%u", (unsigned int)span.source,
        (unsigned int)span.start, (unsigned int)span.end);
}

static const char *cm_hir_dump_usage_name(CmHirValueUsage usage)
{
    switch (usage) {
    case CM_HIR_USAGE_UNKNOWN: return "unknown";
    case CM_HIR_USAGE_BORROW: return "borrow";
    case CM_HIR_USAGE_MUTATE: return "mutate";
    case CM_HIR_USAGE_MOVE: return "move";
    }
    return "invalid";
}

static const char *cm_hir_dump_closure_class_name(CmHirClosureClass class_)
{
    switch (class_) {
    case CM_HIR_CLOSURE_CLASS_UNKNOWN: return "unknown";
    case CM_HIR_CLOSURE_CLASS_NO_CAPTURE: return "no-capture";
    case CM_HIR_CLOSURE_CLASS_SHARED: return "shared";
    case CM_HIR_CLOSURE_CLASS_MUT: return "mut";
    case CM_HIR_CLOSURE_CLASS_ONCE: return "once";
    }
    return "invalid";
}

static void cm_hir_dump_visibility(FILE *stream,
    const CmHirVisibility *visibility)
{
    switch (visibility->kind) {
    case CM_HIR_VIS_PRIVATE:
        fputs("private", stream);
        break;
    case CM_HIR_VIS_PUBLIC:
        fputs("public", stream);
        break;
    case CM_HIR_VIS_CRATE:
        fputs("crate", stream);
        break;
    case CM_HIR_VIS_RESTRICTED:
        fputs("restricted(", stream);
        cm_hir_dump_def(stream, visibility->restriction);
        fputc(')', stream);
        break;
    }
}

static const char *cm_hir_namespace_name(CmHirNamespace namespace_kind)
{
    static const char *const names[] = { "type", "value", "macro" };

    if ((unsigned int)namespace_kind >=
        (unsigned int)CM_ARRAY_LEN(names)) {
        return "bad-namespace";
    }
    return names[(unsigned int)namespace_kind];
}

static const char *cm_hir_primitive_name(CmHirPrimitiveKind kind)
{
    static const char *const names[] = {
        "none", "bool", "char", "str",
        "i8", "i16", "i32", "i64", "i128", "isize",
        "u8", "u16", "u32", "u64", "u128", "usize",
        "f16", "f32", "f64", "f128"
    };

    if ((unsigned int)kind >= (unsigned int)CM_ARRAY_LEN(names))
        return "bad-primitive";
    return names[(unsigned int)kind];
}

static const char *cm_hir_int_name(CmHirIntType kind)
{
    static const char *const names[] = {
        "i8", "i16", "i32", "i64", "i128", "isize",
        "u8", "u16", "u32", "u64", "u128", "usize"
    };

    if ((unsigned int)kind >= (unsigned int)CM_ARRAY_LEN(names)) {
        return "bad-int";
    }
    return names[(unsigned int)kind];
}

static const char *cm_hir_float_name(CmHirFloatType kind)
{
    static const char *const names[] = { "f16", "f32", "f64", "f128" };

    if ((unsigned int)kind >= (unsigned int)CM_ARRAY_LEN(names)) {
        return "bad-float";
    }
    return names[(unsigned int)kind];
}

static unsigned int cm_hir_edition_number(CmHirEdition edition)
{
    switch (edition) {
    case CM_HIR_EDITION_2015:
        return 2015u;
    case CM_HIR_EDITION_2018:
        return 2018u;
    case CM_HIR_EDITION_2021:
        return 2021u;
    case CM_HIR_EDITION_2024:
        return 2024u;
    }
    return 0u;
}

static void cm_hir_dump_region(FILE *stream, const CmHirContext *context,
    const CmHirRegion *region)
{
    switch (region->kind) {
    case CM_HIR_REGION_STATIC:
        fputs("'static", stream);
        break;
    case CM_HIR_REGION_EARLY_BOUND:
        fprintf(stream, "early($%u)",
            (unsigned int)region->data.parameter);
        break;
    case CM_HIR_REGION_LATE_BOUND:
        fprintf(stream, "late(%u)",
            (unsigned int)region->data.binder_index);
        break;
    case CM_HIR_REGION_INFER:
        fprintf(stream, "region?%u",
            (unsigned int)region->data.inference_variable);
        break;
    case CM_HIR_REGION_ERASED:
        fputs("erased", stream);
        break;
    case CM_HIR_REGION_ERROR:
        fputs("region-error(", stream);
        cm_hir_dump_string(stream, context, region->data.error_reason);
        fputc(')', stream);
        break;
    }
}

static void cm_hir_dump_const(FILE *stream, const CmHirContext *context,
    const CmHirConstArg *constant)
{
    switch (constant->kind) {
    case CM_HIR_CONST_VALUE:
        if (constant->data.value.high_bits == 0u) {
            fprintf(stream, "bits=0x%llx",
                (unsigned long long)constant->data.value.low_bits);
        } else {
            fprintf(stream, "bits=0x%llx%016llx",
                (unsigned long long)constant->data.value.high_bits,
                (unsigned long long)constant->data.value.low_bits);
        }
        break;
    case CM_HIR_CONST_PARAMETER:
        fprintf(stream, "const-param=$%u",
            (unsigned int)constant->data.parameter);
        break;
    case CM_HIR_CONST_UNEVALUATED:
        fputs("unevaluated=", stream);
        cm_hir_dump_def(stream, constant->data.definition);
        break;
    case CM_HIR_CONST_INFER:
        fprintf(stream, "const?%u",
            (unsigned int)constant->data.inference_variable);
        break;
    case CM_HIR_CONST_ERROR:
        fputs("const-error(", stream);
        cm_hir_dump_string(stream, context, constant->data.error_reason);
        fputc(')', stream);
        break;
    }
    fprintf(stream, ":ty#%u", (unsigned int)constant->type);
}

static void cm_hir_dump_generic_arg(FILE *stream,
    const CmHirContext *context, const CmHirGenericArg *argument)
{
    switch (argument->kind) {
    case CM_HIR_GENERIC_ARG_LIFETIME:
        cm_hir_dump_region(stream, context, &argument->data.lifetime);
        break;
    case CM_HIR_GENERIC_ARG_TYPE:
        fprintf(stream, "ty#%u", (unsigned int)argument->data.type);
        break;
    case CM_HIR_GENERIC_ARG_CONST:
        cm_hir_dump_const(stream, context, &argument->data.constant);
        break;
    }
}

static void cm_hir_dump_named(FILE *stream, const CmHirContext *context,
    const CmHirNamedType *named)
{
    uint32_t index;

    cm_hir_dump_def(stream, named->definition);
    if (named->argument_count != 0u) {
        fputc('<', stream);
        for (index = 0u; index < named->argument_count; ++index) {
            if (index != 0u) {
                fputc(',', stream);
            }
            cm_hir_dump_generic_arg(stream, context,
                &named->arguments[index]);
        }
        fputc('>', stream);
    }
}

static void cm_hir_dump_type(FILE *stream, const CmHirContext *context,
    const CmHirType *type)
{
    uint32_t index;

    switch (type->kind) {
    case CM_HIR_TYPE_ERROR_KIND:
        fputs("error(", stream);
        cm_hir_dump_string(stream, context, type->data.error_type.reason);
        fputc(')', stream);
        break;
    case CM_HIR_TYPE_INFER_KIND:
        fprintf(stream, "infer[%u]?%u",
            (unsigned int)type->data.infer_type.kind,
            (unsigned int)type->data.infer_type.variable);
        break;
    case CM_HIR_TYPE_NEVER_KIND:
        fputc('!', stream);
        break;
    case CM_HIR_TYPE_UNIT_KIND:
        fputs("unit", stream);
        break;
    case CM_HIR_TYPE_BOOL_KIND:
        fputs("bool", stream);
        break;
    case CM_HIR_TYPE_CHAR_KIND:
        fputs("char", stream);
        break;
    case CM_HIR_TYPE_STR_KIND:
        fputs("str", stream);
        break;
    case CM_HIR_TYPE_INTEGER_KIND:
        fputs(cm_hir_int_name(type->data.integer_type.kind), stream);
        break;
    case CM_HIR_TYPE_FLOAT_KIND:
        fputs(cm_hir_float_name(type->data.float_type.kind), stream);
        break;
    case CM_HIR_TYPE_REFERENCE_KIND:
        fputc('&', stream);
        cm_hir_dump_region(stream, context,
            &type->data.reference_type.region);
        fprintf(stream, " %s ty#%u",
            type->data.reference_type.mutability == CM_HIR_MUTABLE
                ? "mut" : "const",
            (unsigned int)type->data.reference_type.pointee);
        break;
    case CM_HIR_TYPE_RAW_POINTER_KIND:
        fprintf(stream, "*%s ty#%u",
            type->data.raw_pointer_type.mutability == CM_HIR_MUTABLE
                ? "mut" : "const",
            (unsigned int)type->data.raw_pointer_type.pointee);
        break;
    case CM_HIR_TYPE_TUPLE_KIND:
        fputc('(', stream);
        for (index = 0u; index < type->data.tuple_type.element_count;
             ++index) {
            if (index != 0u) {
                fputc(',', stream);
            }
            fprintf(stream, "ty#%u",
                (unsigned int)type->data.tuple_type.elements[index]);
        }
        fputc(')', stream);
        break;
    case CM_HIR_TYPE_ARRAY_KIND:
        fprintf(stream, "[ty#%u;",
            (unsigned int)type->data.array_type.element);
        cm_hir_dump_const(stream, context, &type->data.array_type.length);
        fputc(']', stream);
        break;
    case CM_HIR_TYPE_SLICE_KIND:
        fprintf(stream, "[ty#%u]",
            (unsigned int)type->data.slice_type.element);
        break;
    case CM_HIR_TYPE_FN_POINTER_KIND:
        if (type->data.fn_pointer_type.binder.lifetime_count != 0u) {
            fputs("for<", stream);
            for (index = 0u;
                 index < type->data.fn_pointer_type.binder.lifetime_count;
                 ++index) {
                if (index != 0u) fputc(',', stream);
                cm_hir_dump_string(stream, context,
                    type->data.fn_pointer_type.binder.lifetimes[index]);
            }
            fputs("> ", stream);
        }
        fputs(type->data.fn_pointer_type.safety == CM_HIR_UNSAFE
            ? "unsafe fn[" : "fn[", stream);
        cm_hir_dump_string(stream, context,
            type->data.fn_pointer_type.abi);
        fputs("](", stream);
        for (index = 0u;
             index < type->data.fn_pointer_type.parameter_count; ++index) {
            if (index != 0u) {
                fputc(',', stream);
            }
            fprintf(stream, "ty#%u", (unsigned int)
                type->data.fn_pointer_type.parameters[index]);
        }
        if (type->data.fn_pointer_type.is_variadic) {
            if (type->data.fn_pointer_type.parameter_count != 0u) {
                fputc(',', stream);
            }
            fputs("...", stream);
        }
        fprintf(stream, ")->ty#%u",
            (unsigned int)type->data.fn_pointer_type.return_type);
        break;
    case CM_HIR_TYPE_FN_DEFINITION_KIND:
        fputs("fn-def ", stream);
        cm_hir_dump_named(stream, context, &type->data.named_type);
        break;
    case CM_HIR_TYPE_ADT_KIND:
        fputs("adt ", stream);
        cm_hir_dump_named(stream, context, &type->data.named_type);
        break;
    case CM_HIR_TYPE_ALIAS_APPLICATION_KIND:
        fputs("alias-app ", stream);
        cm_hir_dump_named(stream, context, &type->data.named_type);
        break;
    case CM_HIR_TYPE_SELF_KIND:
        fputs("Self(owner=", stream);
        cm_hir_dump_def(stream, type->data.self_type.owner);
        fputc(')', stream);
        break;
    case CM_HIR_TYPE_PARAMETER_KIND:
        fprintf(stream, "param $%u",
            (unsigned int)type->data.parameter_type.parameter);
        break;
    case CM_HIR_TYPE_PROJECTION_KIND:
        fprintf(stream, "projection <ty#%u as ",
            (unsigned int)type->data.projection_type.self_type);
        cm_hir_dump_named(stream, context,
            &type->data.projection_type.trait_type);
        fputs(">::", stream);
        cm_hir_dump_named(stream, context,
            &type->data.projection_type.associated_type);
        break;
    case CM_HIR_TYPE_DYN_TRAIT_KIND:
        fputs("dyn ", stream);
        if (type->data.dyn_trait_type.has_principal) {
            cm_hir_dump_named(stream, context,
                &type->data.dyn_trait_type.principal_trait);
        }
        for (index = 0u;
             index < type->data.dyn_trait_type.equality_count; ++index) {
            fputs("{assoc=", stream);
            cm_hir_dump_def(stream,
                type->data.dyn_trait_type.equalities[index]
                    .associated_type);
            fprintf(stream, ",value=ty#%u}",
                (unsigned int)type->data.dyn_trait_type
                    .equalities[index].value);
        }
        for (index = 0u;
             index < type->data.dyn_trait_type.auto_trait_count; ++index) {
            if (type->data.dyn_trait_type.has_principal || index != 0u) {
                fputc('+', stream);
            }
            cm_hir_dump_named(stream, context,
                &type->data.dyn_trait_type.auto_traits[index]);
        }
        fputc('+', stream);
        cm_hir_dump_region(stream, context,
            &type->data.dyn_trait_type.region);
        break;
    case CM_HIR_TYPE_OPAQUE_KIND:
        fputs("opaque ", stream);
        cm_hir_dump_named(stream, context, &type->data.named_type);
        break;
    case CM_HIR_TYPE_CLOSURE_KIND:
        fprintf(stream, "closure#%u",
            (unsigned int)type->data.closure_type.closure);
        break;
    case CM_HIR_TYPE_FOREIGN_KIND:
        fputs("foreign ", stream);
        cm_hir_dump_named(stream, context, &type->data.named_type);
        break;
    }
}

static const char *cm_hir_item_name(CmHirItemKind kind)
{
    static const char *const names[] = {
        "function", "struct", "union", "enum", "type-alias", "const",
        "static", "module", "trait", "impl", "extern-type",
        "trait-alias"
    };

    if ((unsigned int)kind >= (unsigned int)CM_ARRAY_LEN(names)) {
        return "bad-item";
    }
    return names[(unsigned int)kind];
}

static const char *cm_hir_receiver_name(CmHirReceiverKind kind)
{
    static const char *const names[] = {
        "none", "value", "ref-shared", "ref-mutable", "custom"
    };

    if ((unsigned int)kind >= (unsigned int)CM_ARRAY_LEN(names)) {
        return "bad-receiver";
    }
    return names[(unsigned int)kind];
}

static const char *cm_hir_binding_name(CmHirBindingKind kind)
{
    static const char *const names[] = {
        "named", "discard", "tuple-pattern", "newtype-pattern"
    };

    if ((unsigned int)kind >= (unsigned int)CM_ARRAY_LEN(names)) {
        return "bad-binding";
    }
    return names[(unsigned int)kind];
}

static const char *cm_hir_parameter_binding_mode_name(
    CmHirParameterBindingMode mode)
{
    static const char *const names[] = {
        "move", "ref", "ref-mut", "deref-shared"
    };

    if ((unsigned int)mode >= (unsigned int)CM_ARRAY_LEN(names)) {
        return "bad-mode";
    }
    return names[(unsigned int)mode];
}

static const char *cm_hir_supertrait_modifier_name(
    CmHirSupertraitModifier modifier)
{
    static const char *const names[] = { "required", "const-if-const" };

    if ((unsigned int)modifier >= (unsigned int)CM_ARRAY_LEN(names)) {
        return "bad-supertrait-modifier";
    }
    return names[(unsigned int)modifier];
}

static const char *cm_hir_associated_bound_modifier_name(
    CmHirAssociatedTypeBoundModifier modifier)
{
    static const char *const names[] = { "required", "relaxed" };

    if ((unsigned int)modifier >= (unsigned int)CM_ARRAY_LEN(names)) {
        return "bad-associated-bound-modifier";
    }
    return names[(unsigned int)modifier];
}

int cm_hir_dump(FILE *stream, const CmHirContext *context)
{
    size_t index;

    if (stream == NULL || context == NULL) {
        return -1;
    }
    fputs("hir-v33\n", stream);
    for (index = 0u; index < context->crates.len; ++index) {
        const CmHirCrate *crate_value;

        crate_value = (const CmHirCrate *)cm_vec_at_const(&context->crates,
            index);
        fprintf(stream, "crate#%u name=", (unsigned int)(index + 1u));
        cm_hir_dump_string(stream, context, crate_value->name);
        fprintf(stream, " edition=%u root=module#%u span=",
            cm_hir_edition_number(crate_value->edition),
            (unsigned int)crate_value->root_module);
        cm_hir_dump_span(stream, crate_value->span);
        fprintf(stream, " inner-attrs=%u",
            (unsigned int)crate_value->inner_attribute_count);
        fputc('\n', stream);
        {
            uint32_t attribute_index;

            for (attribute_index = 0u;
                 attribute_index < crate_value->inner_attribute_count;
                 ++attribute_index) {
                const CmHirAttribute *attribute;

                attribute = &crate_value->inner_attributes[attribute_index];
                fprintf(stream,
                    "crate-attr crate#%u source-attr=%u depth=%u meta=",
                    (unsigned int)(index + 1u),
                    (unsigned int)attribute->source_attribute,
                    (unsigned int)attribute->expansion_depth);
                cm_hir_dump_string(stream, context, attribute->metadata);
                fputs(" span=", stream);
                cm_hir_dump_span(stream, attribute->span);
                fputc('\n', stream);
            }
        }
    }
    for (index = 0u; index < context->modules.len; ++index) {
        const CmHirModule *module;

        module = (const CmHirModule *)cm_vec_at_const(&context->modules,
            index);
        fprintf(stream, "module#%u crate#%u parent=module#%u def=",
            (unsigned int)(index + 1u), (unsigned int)module->crate_id,
            (unsigned int)module->parent);
        cm_hir_dump_def(stream, module->definition);
        fputs(" name=", stream);
        cm_hir_dump_string(stream, context, module->name);
        fputs(" span=", stream);
        cm_hir_dump_span(stream, module->span);
        fprintf(stream, " outer-attrs=%u inner-attrs=%u imports=%u",
            (unsigned int)module->outer_attribute_count,
            (unsigned int)module->inner_attribute_count,
            (unsigned int)module->import_count);
        fputc('\n', stream);
        {
            uint32_t attribute_index;

            for (attribute_index = 0u;
                 attribute_index < module->outer_attribute_count;
                 ++attribute_index) {
                const CmHirAttribute *attribute;

                attribute = &module->outer_attributes[attribute_index];
                fprintf(stream,
                    "module-outer-attr module#%u source-attr=%u depth=%u "
                    "meta=",
                    (unsigned int)(index + 1u),
                    (unsigned int)attribute->source_attribute,
                    (unsigned int)attribute->expansion_depth);
                cm_hir_dump_string(stream, context, attribute->metadata);
                fputs(" span=", stream);
                cm_hir_dump_span(stream, attribute->span);
                fputc('\n', stream);
            }

            for (attribute_index = 0u;
                 attribute_index < module->inner_attribute_count;
                 ++attribute_index) {
                const CmHirAttribute *attribute;

                attribute = &module->inner_attributes[attribute_index];
                fprintf(stream,
                    "module-inner-attr module#%u source-attr=%u depth=%u "
                    "meta=",
                    (unsigned int)(index + 1u),
                    (unsigned int)attribute->source_attribute,
                    (unsigned int)attribute->expansion_depth);
                cm_hir_dump_string(stream, context, attribute->metadata);
                fputs(" span=", stream);
                cm_hir_dump_span(stream, attribute->span);
                fputc('\n', stream);
            }
        }
        {
            uint32_t import_index;

            for (import_index = 0u; import_index < module->import_count;
                 ++import_index) {
                const CmHirImport *import_value;
                uint32_t attribute_index;
                uint32_t binding_index;

                import_value = &module->imports[import_index];
                fprintf(stream,
                    "import module#%u index=%u source-item=%u visibility=",
                    (unsigned int)(index + 1u),
                    (unsigned int)import_index,
                    (unsigned int)import_value->source_item);
                cm_hir_dump_visibility(stream, &import_value->visibility);
                fprintf(stream, " kind=%s tree=",
                    import_value->kind == CM_HIR_IMPORT_USE
                        ? "use" : "extern-crate");
                cm_hir_dump_string(stream, context, import_value->tree);
                fprintf(stream, " attrs=%u bindings=%u span=",
                    (unsigned int)import_value->attribute_count,
                    (unsigned int)import_value->binding_count);
                cm_hir_dump_span(stream, import_value->span);
                fputc('\n', stream);
                for (attribute_index = 0u;
                     attribute_index < import_value->attribute_count;
                     ++attribute_index) {
                    const CmHirAttribute *attribute;

                    attribute = &import_value->attributes[attribute_index];
                    fprintf(stream,
                        "import-attr module#%u index=%u source-attr=%u "
                        "depth=%u meta=",
                        (unsigned int)(index + 1u),
                        (unsigned int)import_index,
                        (unsigned int)attribute->source_attribute,
                        (unsigned int)attribute->expansion_depth);
                    cm_hir_dump_string(stream, context,
                        attribute->metadata);
                    fputs(" span=", stream);
                    cm_hir_dump_span(stream, attribute->span);
                    fputc('\n', stream);
                }
                for (binding_index = 0u;
                     binding_index < import_value->binding_count;
                     ++binding_index) {
                    const CmHirImportBinding *binding;

                    binding = &import_value->bindings[binding_index];
                    fprintf(stream,
                        "import-binding module#%u index=%u binding=%u "
                        "namespace=%s name=",
                        (unsigned int)(index + 1u),
                        (unsigned int)import_index,
                        (unsigned int)binding_index,
                        cm_hir_namespace_name(binding->namespace_kind));
                    cm_hir_dump_string(stream, context, binding->name);
                    fputs(" target=", stream);
                    cm_hir_dump_def(stream, binding->target);
                    if (binding->primitive_kind != CM_HIR_PRIMITIVE_NONE) {
                        fprintf(stream, " primitive=%s",
                            cm_hir_primitive_name(
                                binding->primitive_kind));
                    }
                    fprintf(stream, " anonymous=%u\n",
                        (unsigned int)binding->is_anonymous);
                }
            }
        }
    }
    for (index = 0u; index < context->definitions.len; ++index) {
        const CmHirDefinition *definition;

        definition = (const CmHirDefinition *)cm_vec_at_const(
            &context->definitions, index);
        fputs("def ", stream);
        cm_hir_dump_def(stream, definition->id);
        fprintf(stream, " %s %s ",
            definition->kind == CM_HIR_DEFINITION_MODULE
                ? "module"
                : (definition->kind == CM_HIR_DEFINITION_ITEM
                    ? "item"
                    : (definition->kind
                            == CM_HIR_DEFINITION_ENUM_VARIANT
                        ? "enum-variant" : "macro")),
            definition->state == CM_HIR_DEFINITION_BOUND
                ? "bound" : "reserved");
        if (definition->kind == CM_HIR_DEFINITION_ENUM_VARIANT
            && definition->state == CM_HIR_DEFINITION_BOUND) {
            fprintf(stream, "enum-item#%u variant=%u span=",
                (unsigned int)definition->entity.enum_variant.enum_item_id,
                (unsigned int)definition->entity.enum_variant.variant_index);
        } else if (definition->kind == CM_HIR_DEFINITION_MACRO
            && definition->state == CM_HIR_DEFINITION_BOUND) {
            fprintf(stream, "module#%u form=%s name=",
                (unsigned int)definition->entity.macro_definition
                    .owner_module,
                definition->entity.macro_definition.form
                        == CM_HIR_MACRO_RULES_DEFINITION
                    ? "macro-rules" : "declarative");
            cm_hir_dump_string(stream, context,
                definition->entity.macro_definition.name);
            fputs(" span=", stream);
        } else {
            fprintf(stream, "entity#%u span=",
                definition->state == CM_HIR_DEFINITION_RESERVED ? 0u
                    : (definition->kind == CM_HIR_DEFINITION_MODULE
                        ? (unsigned int)definition->entity.module_id
                        : (unsigned int)definition->entity.item_id));
        }
        cm_hir_dump_span(stream, definition->span);
        fputc('\n', stream);
    }
    for (index = 0u; index < context->generic_parameters.len; ++index) {
        const CmHirGenericParam *parameter;

        parameter = (const CmHirGenericParam *)cm_vec_at_const(
            &context->generic_parameters, index);
        fprintf(stream, "generic#%u owner=", (unsigned int)(index + 1u));
        cm_hir_dump_def(stream, parameter->owner);
        fprintf(stream, " index=%u kind=%u name=",
            (unsigned int)parameter->index, (unsigned int)parameter->kind);
        cm_hir_dump_string(stream, context, parameter->name);
        fprintf(stream, " declared=ty#%u relaxed-sized=%d default=",
            (unsigned int)parameter->declared_type,
            parameter->is_relaxed_sized);
        if (parameter->has_default) {
            cm_hir_dump_generic_arg(stream, context,
                &parameter->default_argument);
        } else {
            fputs("none", stream);
        }
        fputs(" span=", stream);
        cm_hir_dump_span(stream, parameter->span);
        fputc('\n', stream);
    }
    for (index = 0u; index < context->types.len; ++index) {
        const CmHirType *type;

        type = (const CmHirType *)cm_vec_at_const(&context->types, index);
        fprintf(stream, "type#%u ", (unsigned int)(index + 1u));
        cm_hir_dump_type(stream, context, type);
        fputs(" span=", stream);
        cm_hir_dump_span(stream, type->span);
        fputc('\n', stream);
    }
    for (index = 0u; index < context->closures.len; ++index) {
        const CmHirClosure *closure;
        uint32_t capture_index;
        uint32_t parameter_index;

        closure = (const CmHirClosure *)cm_vec_at_const(
            &context->closures, index);
        fprintf(stream,
            "closure#%u state=%s owner=body#%u source-expr=%u "
            "return=ty#%u body=",
            (unsigned int)(index + 1u),
            closure->state == CM_HIR_CLOSURE_SIGNATURE_RESERVED
                ? "signature-reserved" : "body-bound",
            (unsigned int)closure->owner_body,
            (unsigned int)closure->source_expression_id,
            (unsigned int)closure->return_type);
        if (closure->body_expression == CM_HIR_EXPR_NONE) {
            fputs("none", stream);
        } else {
            fprintf(stream, "expr#%u",
                (unsigned int)closure->body_expression);
        }
        fprintf(stream,
            " visible-locals=%u move=%d captures=%s class=%s copy=%d "
            "capture-values=[",
            (unsigned int)closure->visible_local_count, closure->is_move,
            closure->capture_state == CM_HIR_CLOSURE_CAPTURES_UNMARKED
                ? "unmarked"
                : closure->capture_state == CM_HIR_CLOSURE_CAPTURES_MARKED
                    ? "marked" : "invalid",
            cm_hir_dump_closure_class_name(closure->callable_class),
            closure->is_copy);
        if (closure->captures == NULL && closure->capture_count != 0u) {
            fputs("invalid", stream);
        } else {
            for (capture_index = 0u;
                 capture_index < closure->capture_count; ++capture_index) {
                const CmHirClosureCapture *capture;

                capture = &closure->captures[capture_index];
                if (capture_index != 0u) fputc(',', stream);
                fprintf(stream, "capture(local=%u,type=ty#%u,usage=%s)",
                    (unsigned int)capture->local_index,
                    (unsigned int)capture->type,
                    cm_hir_dump_usage_name(capture->usage));
            }
        }
        fputs("] parameters=[", stream);
        for (parameter_index = 0u;
             parameter_index < closure->parameter_count;
             ++parameter_index) {
            const CmHirClosureParam *parameter;

            parameter = &closure->parameters[parameter_index];
            if (parameter_index != 0u) fputc(',', stream);
            fprintf(stream, "parameter(index=%u,kind=%s,name=",
                (unsigned int)parameter_index,
                parameter->binding_kind == CM_HIR_BINDING_NAMED
                    ? "named" : "discard");
            if (parameter->binding_kind == CM_HIR_BINDING_DISCARD) {
                fputs("_", stream);
            } else {
                cm_hir_dump_string(stream, context, parameter->name);
            }
            fprintf(stream, ",type=ty#%u,span=",
                (unsigned int)parameter->type);
            cm_hir_dump_span(stream, parameter->span);
            fputc(')', stream);
        }
        fputs("] span=", stream);
        cm_hir_dump_span(stream, closure->span);
        fputc('\n', stream);
    }
    for (index = 0u; index < context->expressions.len; ++index) {
        const CmHirExpr *expression;
        const char *kind_name;
        uint32_t child_index;

        expression = (const CmHirExpr *)cm_vec_at_const(
            &context->expressions, index);
        switch (expression->kind) {
        case CM_HIR_EXPR_INTEGER: kind_name = "integer"; break;
        case CM_HIR_EXPR_BLOCK: kind_name = "block"; break;
        case CM_HIR_EXPR_LOCAL: kind_name = "local"; break;
        case CM_HIR_EXPR_CALL: kind_name = "call"; break;
        case CM_HIR_EXPR_METHOD_CALL: kind_name = "method-call"; break;
        case CM_HIR_EXPR_QUALIFIED_CALL:
            kind_name = "qualified-call";
            break;
        case CM_HIR_EXPR_BINARY: kind_name = "binary"; break;
        case CM_HIR_EXPR_AGGREGATE: kind_name = "aggregate"; break;
        case CM_HIR_EXPR_FIELD: kind_name = "field"; break;
        case CM_HIR_EXPR_IF: kind_name = "if"; break;
        case CM_HIR_EXPR_BORROW_SHARED:
            kind_name = "borrow-shared";
            break;
        case CM_HIR_EXPR_DEREFERENCE: kind_name = "dereference"; break;
        case CM_HIR_EXPR_CLOSURE_PARAMETER:
            kind_name = "closure-parameter";
            break;
        case CM_HIR_EXPR_CLOSURE: kind_name = "closure"; break;
        default: kind_name = "unknown"; break;
        }
        fprintf(stream, "expr#%u %s type=ty#%u ",
            (unsigned int)(index + 1u),
            kind_name,
            (unsigned int)expression->type);
        switch (expression->kind) {
        case CM_HIR_EXPR_INTEGER:
            fprintf(stream, "bits=0x%016llx:%016llx",
                (unsigned long long)expression->data.integer.high_bits,
                (unsigned long long)expression->data.integer.low_bits);
            break;
        case CM_HIR_EXPR_BLOCK:
            fputs("statements=[", stream);
            for (child_index = 0u;
                 child_index < expression->data.block.statement_count;
                 ++child_index) {
                const CmHirStatement *statement;

                statement = &expression->data.block.statements[child_index];
                if (child_index != 0u) fputc(',', stream);
                if (statement->kind == CM_HIR_STATEMENT_LET) {
                    fprintf(stream, "let(local=%u,initializer=expr#%u,span=",
                        (unsigned int)statement->data.let_statement
                            .local_index,
                        (unsigned int)statement->data.let_statement
                            .initializer);
                    cm_hir_dump_span(stream, statement->span);
                    fputc(')', stream);
                } else {
                    fputs("invalid", stream);
                }
            }
            fprintf(stream, "] tail=expr#%u",
                (unsigned int)expression->data.block.tail_expression);
            break;
        case CM_HIR_EXPR_LOCAL:
            fprintf(stream, "local=%u",
                (unsigned int)expression->data.local.local_index);
            break;
        case CM_HIR_EXPR_CLOSURE_PARAMETER:
            fprintf(stream, "closure=closure#%u parameter=%u",
                (unsigned int)expression->data.closure_parameter.closure,
                (unsigned int)expression->data.closure_parameter
                    .parameter_index);
            break;
        case CM_HIR_EXPR_CLOSURE:
            fprintf(stream, "closure=closure#%u",
                (unsigned int)expression->data.closure.closure);
            break;
        case CM_HIR_EXPR_CALL:
            fputs("callee=", stream);
            cm_hir_dump_def(stream, expression->data.call.callee);
            fputs(" substitutions=[", stream);
            for (child_index = 0u;
                 child_index < expression->data.call.type_substitution_count;
                 ++child_index) {
                if (child_index != 0u) fputc(',', stream);
                fprintf(stream, "ty#%u", (unsigned int)
                    expression->data.call.type_substitutions[child_index]);
            }
            fputs("] arguments=[", stream);
            for (child_index = 0u;
                 child_index < expression->data.call.argument_count;
                 ++child_index) {
                if (child_index != 0u) fputc(',', stream);
                fprintf(stream, "expr#%u", (unsigned int)
                    expression->data.call.arguments[child_index]);
            }
            fputc(']', stream);
            break;
        case CM_HIR_EXPR_METHOD_CALL:
            fputs("syntax=dot-method name=", stream);
            cm_hir_dump_string(stream, context,
                expression->data.method_call.method_name);
            fprintf(stream, " receiver=expr#%u arguments=[",
                (unsigned int)expression->data.method_call.receiver);
            for (child_index = 0u;
                 child_index < expression->data.method_call.argument_count;
                 ++child_index) {
                if (child_index != 0u) fputc(',', stream);
                fprintf(stream, "expr#%u", (unsigned int)
                    expression->data.method_call.arguments[child_index]);
            }
            fputs("] traits=[", stream);
            for (child_index = 0u;
                 child_index
                    < expression->data.method_call.in_scope_trait_count;
                 ++child_index) {
                if (child_index != 0u) fputc(',', stream);
                cm_hir_dump_def(stream,
                    expression->data.method_call.in_scope_traits[
                        child_index]);
            }
            fputc(']', stream);
            break;
        case CM_HIR_EXPR_QUALIFIED_CALL:
            fputs("syntax=qualified-trait-method self=ty#", stream);
            fprintf(stream, "%u trait=",
                (unsigned int)expression->data.qualified_call
                    .requested_self_type);
            cm_hir_dump_def(stream,
                expression->data.qualified_call.requested_trait);
            fputs(" declared=", stream);
            cm_hir_dump_def(stream,
                expression->data.qualified_call.declared_trait_callable);
            if (expression->data.qualified_call.receiver_argument
                    == CM_HIR_CALLABLE_RECEIVER_NONE) {
                fputs(" receiver=none", stream);
            } else {
                fprintf(stream, " receiver=argument[%u]",
                    (unsigned int)expression->data.qualified_call
                        .receiver_argument);
            }
            fputs(" arguments=[", stream);
            for (child_index = 0u;
                 child_index
                    < expression->data.qualified_call.argument_count;
                 ++child_index) {
                if (child_index != 0u) fputc(',', stream);
                fprintf(stream, "expr#%u", (unsigned int)
                    expression->data.qualified_call.arguments[child_index]);
            }
            fputc(']', stream);
            break;
        case CM_HIR_EXPR_BINARY:
            fprintf(stream, "operator=%s left=expr#%u right=expr#%u",
                expression->data.binary.operator_kind == CM_HIR_BINARY_ADD
                    ? "add"
                    : expression->data.binary.operator_kind
                            == CM_HIR_BINARY_SUBTRACT
                        ? "subtract"
                        : expression->data.binary.operator_kind
                                == CM_HIR_BINARY_EQUAL
                            ? "equal"
                            : expression->data.binary.operator_kind
                                    == CM_HIR_BINARY_LESS
                                ? "less" : "unknown",
                (unsigned int)expression->data.binary.left,
                (unsigned int)expression->data.binary.right);
            break;
        case CM_HIR_EXPR_AGGREGATE:
            fputs("aggregate=", stream);
            cm_hir_dump_def(stream, expression->data.aggregate.definition);
            fputs(" fields=[", stream);
            for (child_index = 0u;
                 child_index < expression->data.aggregate.field_count;
                 ++child_index) {
                const CmHirAggregateFieldValue *field_value;

                field_value = &expression->data.aggregate.fields[child_index];
                if (child_index != 0u) fputc(',', stream);
                fprintf(stream, "field(index=%u,value=expr#%u,span=",
                    (unsigned int)field_value->field_index,
                    (unsigned int)field_value->value);
                cm_hir_dump_span(stream, field_value->span);
                fputc(')', stream);
            }
            fputc(']', stream);
            break;
        case CM_HIR_EXPR_FIELD:
            fprintf(stream, "base=expr#%u definition=",
                (unsigned int)expression->data.field.base);
            cm_hir_dump_def(stream, expression->data.field.definition);
            fprintf(stream, " field=%u",
                (unsigned int)expression->data.field.field_index);
            break;
        case CM_HIR_EXPR_IF:
            fprintf(stream,
                "condition=expr#%u then=expr#%u else=expr#%u",
                (unsigned int)expression->data.if_expr.condition,
                (unsigned int)expression->data.if_expr.then_expression,
                (unsigned int)expression->data.if_expr.else_expression);
            break;
        case CM_HIR_EXPR_BORROW_SHARED:
            fprintf(stream, "operand=expr#%u",
                (unsigned int)expression->data.borrow_shared.operand);
            break;
        case CM_HIR_EXPR_DEREFERENCE:
            fprintf(stream, "operand=expr#%u",
                (unsigned int)expression->data.dereference.operand);
            break;
        default:
            fputs("invalid", stream);
            break;
        }
        if (expression->owner_body == CM_HIR_BODY_NONE) {
            fputs(" owner=none", stream);
        } else {
            fprintf(stream, " owner=body#%u",
                (unsigned int)expression->owner_body);
        }
        fputs(" span=", stream);
        cm_hir_dump_span(stream, expression->span);
        fprintf(stream, " usage=%s static-borrow=%s",
            cm_hir_dump_usage_name(expression->usage),
            expression->static_borrow_state
                    == CM_HIR_STATIC_BORROW_UNKNOWN
                ? "unknown"
                : expression->static_borrow_state
                        == CM_HIR_STATIC_BORROW_NOT_PROMOTED
                    ? "not-promoted"
                    : expression->static_borrow_state
                            == CM_HIR_STATIC_BORROW_PROMOTED
                        ? "promoted" : "invalid");
        fputc('\n', stream);
    }
    for (index = 0u; index < context->bodies.len; ++index) {
        const CmHirBody *body;
        uint32_t local_index;

        body = (const CmHirBody *)cm_vec_at_const(&context->bodies, index);
        fprintf(stream, "body#%u owner=", (unsigned int)(index + 1u));
        cm_hir_dump_def(stream, body->owner);
        fputs(" origin=", stream);
        fputs(body->origin.kind == CM_HIR_BODY_ORIGIN_ITEM_SOURCE
                ? "item-source" : "invalid",
            stream);
        fputs(" definition=", stream);
        cm_hir_dump_def(stream, body->origin.definition);
        fputs(" enclosing=", stream);
        cm_hir_dump_def(stream, body->origin.enclosing_definition);
        fputs(" item=", stream);
        cm_hir_dump_def(stream,
            body->origin.data.item_source.item_definition);
        fprintf(stream, " state=%s expected=ty#%u locals=%u params=%u ",
            body->state == CM_HIR_BODY_UNLOWERED ? "unlowered"
                : (body->state == CM_HIR_BODY_TYPED ? "typed" : "error"),
            (unsigned int)body->expected_type,
            (unsigned int)body->local_count,
            (unsigned int)body->parameter_count);
        if (body->state == CM_HIR_BODY_UNLOWERED) {
            fprintf(stream, "source-expr=%u:%u",
                (unsigned int)body->source,
                (unsigned int)body->source_expression_id);
        } else if (body->state == CM_HIR_BODY_TYPED) {
            fprintf(stream, "source-expr=%u:%u root=expr#%u",
                (unsigned int)body->source,
                (unsigned int)body->source_expression_id,
                (unsigned int)body->root_expression);
        } else {
            fputs("reason=", stream);
            cm_hir_dump_string(stream, context, body->error_reason);
        }
        fputs(" span=", stream);
        cm_hir_dump_span(stream, body->span);
        fputc('\n', stream);
        for (local_index = 0u; local_index < body->local_count;
             ++local_index) {
            const CmHirLocal *local;

            local = &body->locals[local_index];
            fprintf(stream, "body-local body#%u index=%u origin=",
                (unsigned int)(index + 1u), (unsigned int)local_index);
            if (local->parameter_index == CM_HIR_PARAMETER_INDEX_NONE) {
                fputs("local", stream);
            } else {
                fprintf(stream, "parameter[%u].binding[%u]",
                    (unsigned int)local->parameter_index,
                    (unsigned int)local->parameter_binding_index);
            }
            fputs(" name=", stream);
            cm_hir_dump_string(stream, context, local->name);
            fprintf(stream, " mutability=%s type=ty#%u span=",
                local->mutability == CM_HIR_MUTABLE
                    ? "mutable" : "immutable",
                (unsigned int)local->type);
            cm_hir_dump_span(stream, local->span);
            fputc('\n', stream);
        }
    }
    for (index = 0u; index < context->items.len; ++index) {
        const CmHirItem *item;

        item = (const CmHirItem *)cm_vec_at_const(&context->items, index);
        fprintf(stream, "item#%u %s def=", (unsigned int)(index + 1u),
            cm_hir_item_name(item->kind));
        cm_hir_dump_def(stream, item->definition);
        fprintf(stream, " module#%u parent=", (unsigned int)item->owner_module);
        cm_hir_dump_def(stream, item->parent_definition);
        fputs(" name=", stream);
        if (item->kind == CM_HIR_ITEM_IMPL) {
            fputs("none", stream);
        } else {
            cm_hir_dump_string(stream, context, item->name);
        }
        if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            fputs(" trait-item=", stream);
            cm_hir_dump_def(stream,
                item->data.type_alias_item.trait_item_definition);
        }
        if (item->kind == CM_HIR_ITEM_FUNCTION) {
            fputs(" trait-item=", stream);
            cm_hir_dump_def(stream,
                item->data.function_item.trait_item_definition);
            fprintf(stream, " receiver=%s", cm_hir_receiver_name(
                item->data.function_item.signature.receiver));
            fprintf(stream, " default-body=%d",
                item->data.function_item.has_default_body);
        }
        if (item->kind == CM_HIR_ITEM_CONST
            || item->kind == CM_HIR_ITEM_STATIC) {
            fputs(" trait-item=", stream);
            cm_hir_dump_def(stream,
                item->data.value_item.trait_item_definition);
            fprintf(stream, " default-body=%d",
                item->data.value_item.has_default_body);
        }
        fprintf(stream, " generics=%u..%u",
            (unsigned int)item->generic_parameter_start,
            (unsigned int)(item->generic_parameter_start
                + item->generic_parameter_count));
        fprintf(stream, " specializable=%d", item->is_specializable);
        fprintf(stream, " attrs=%u span=",
            (unsigned int)item->attribute_count);
        cm_hir_dump_span(stream, item->span);
        fputc('\n', stream);
        if (item->kind == CM_HIR_ITEM_TRAIT) {
            fprintf(stream,
                "trait-header item#%u safety=%s auto=%d const=%d\n",
                (unsigned int)(index + 1u),
                item->data.trait_item.safety == CM_HIR_UNSAFE
                    ? "unsafe" : "safe",
                item->data.trait_item.is_auto,
                item->data.trait_item.is_const);
        }
        if (item->kind == CM_HIR_ITEM_IMPL) {
            fprintf(stream,
                "impl-header item#%u safety=%s negative=%d const=%d "
                "self=ty#%u trait=",
                (unsigned int)(index + 1u),
                item->data.impl_item.safety == CM_HIR_UNSAFE
                    ? "unsafe" : "safe",
                item->data.impl_item.is_negative,
                item->data.impl_item.is_const,
                (unsigned int)item->data.impl_item.self_type);
            if (item->data.impl_item.has_trait) {
                cm_hir_dump_named(stream, context,
                    &item->data.impl_item.trait_type);
            } else {
                fputs("none", stream);
            }
            fputc('\n', stream);
        }
        {
            uint32_t attribute_index;

            for (attribute_index = 0u; attribute_index < item->attribute_count;
                 ++attribute_index) {
                const CmHirAttribute *attribute;

                attribute = &item->attributes[attribute_index];
                fprintf(stream,
                    "item-attr item#%u source-attr=%u depth=%u meta=",
                    (unsigned int)(index + 1u),
                    (unsigned int)attribute->source_attribute,
                    (unsigned int)attribute->expansion_depth);
                cm_hir_dump_string(stream, context, attribute->metadata);
                fputs(" span=", stream);
                cm_hir_dump_span(stream, attribute->span);
                fputc('\n', stream);
            }
        }
        {
            uint32_t predicate_index;

            for (predicate_index = 0u;
                 predicate_index < item->predicate_scope_count;
                 ++predicate_index) {
                const CmHirPredicateScope *scope;
                uint32_t binder_index;

                scope = &item->predicate_scopes[predicate_index];
                fprintf(stream,
                    "predicate-scope item#%u index=%u subject=",
                    (unsigned int)(index + 1u),
                    (unsigned int)predicate_index);
                if (scope->subject_kind == CM_HIR_OUTLIVES_TYPE) {
                    fprintf(stream, "ty#%u",
                        (unsigned int)scope->subject.type);
                } else {
                    cm_hir_dump_region(stream, context,
                        &scope->subject.lifetime);
                }
                fputs(" binder=for<", stream);
                for (binder_index = 0u;
                     binder_index < scope->binder.lifetime_count;
                     ++binder_index) {
                    if (binder_index != 0u) fputc(',', stream);
                    cm_hir_dump_string(stream, context,
                        scope->binder.lifetimes[binder_index]);
                }
                fputs("> binder-span=", stream);
                cm_hir_dump_span(stream, scope->binder.span);
                fprintf(stream, " traits=%u outlives=%u span=",
                    (unsigned int)scope->trait_predicate_count,
                    (unsigned int)scope->outlives_predicate_count);
                cm_hir_dump_span(stream, scope->span);
                fputc('\n', stream);
            }

            for (predicate_index = 0u;
                 predicate_index < item->predicate_count;
                 ++predicate_index) {
                const CmHirTraitPredicate *predicate;

                predicate = &item->predicates[predicate_index];
                fprintf(stream,
                    "trait-predicate item#%u index=%u subject=ty#%u ",
                    (unsigned int)(index + 1u),
                    (unsigned int)predicate_index,
                    (unsigned int)predicate->subject);
                if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE) {
                    fprintf(stream, "scope=%u ",
                        (unsigned int)predicate->scope);
                }
                if (predicate->binder.lifetime_count != 0u) {
                    uint32_t binder_index;

                    fputs("binder=for<", stream);
                    for (binder_index = 0u;
                         binder_index < predicate->binder.lifetime_count;
                         ++binder_index) {
                        if (binder_index != 0u) fputc(',', stream);
                        cm_hir_dump_string(stream, context,
                            predicate->binder.lifetimes[binder_index]);
                    }
                    fputs("> binder-span=", stream);
                    cm_hir_dump_span(stream, predicate->binder.span);
                    fputc(' ', stream);
                }
                fputs("trait=", stream);
                cm_hir_dump_named(stream, context, &predicate->trait_type);
                fprintf(stream, " modifier=%s equalities=%u span=",
                    predicate->modifier == CM_HIR_PREDICATE_CONST_IF_CONST
                        ? "const-if-const"
                        : predicate->modifier == CM_HIR_PREDICATE_CONST
                            ? "const" : "required",
                    (unsigned int)predicate->equality_count);
                cm_hir_dump_span(stream, predicate->span);
                fputc('\n', stream);
                {
                    uint32_t equality_index;

                    for (equality_index = 0u;
                         equality_index < predicate->equality_count;
                         ++equality_index) {
                        const CmHirAssociatedTypeEquality *equality;

                        equality = &predicate->equalities[equality_index];
                        fprintf(stream,
                            "trait-predicate-equality item#%u predicate=%u "
                            "index=%u associated=",
                            (unsigned int)(index + 1u),
                            (unsigned int)predicate_index,
                            (unsigned int)equality_index);
                        cm_hir_dump_def(stream, equality->associated_type);
                        fprintf(stream, " value=ty#%u span=",
                            (unsigned int)equality->value);
                        cm_hir_dump_span(stream, equality->span);
                        fputc('\n', stream);
                    }
                }
            }
        }
        {
            uint32_t predicate_index;

            for (predicate_index = 0u;
                 predicate_index < item->outlives_predicate_count;
                 ++predicate_index) {
                const CmHirOutlivesPredicate *predicate;

                predicate = &item->outlives_predicates[predicate_index];
                fprintf(stream,
                    "outlives-predicate item#%u index=%u subject=",
                    (unsigned int)(index + 1u),
                    (unsigned int)predicate_index);
                if (predicate->subject_kind == CM_HIR_OUTLIVES_TYPE) {
                    fprintf(stream, "ty#%u",
                        (unsigned int)predicate->subject.type);
                } else {
                    cm_hir_dump_region(stream, context,
                        &predicate->subject.lifetime);
                }
                if (predicate->scope != CM_HIR_PREDICATE_SCOPE_NONE) {
                    fprintf(stream, " scope=%u",
                        (unsigned int)predicate->scope);
                }
                fputs(" bound=", stream);
                cm_hir_dump_region(stream, context, &predicate->bound);
                fputs(" span=", stream);
                cm_hir_dump_span(stream, predicate->span);
                fputc('\n', stream);
            }
        }
        if (item->kind == CM_HIR_ITEM_TRAIT) {
            uint32_t supertrait_index;

            for (supertrait_index = 0u;
                 supertrait_index < item->data.trait_item.supertrait_count;
                 ++supertrait_index) {
                const CmHirSupertrait *supertrait;

                supertrait =
                    &item->data.trait_item.supertraits[supertrait_index];
                fprintf(stream,
                    "supertrait item#%u index=%u modifier=%s trait=",
                    (unsigned int)(index + 1u),
                    (unsigned int)supertrait_index,
                    cm_hir_supertrait_modifier_name(supertrait->modifier));
                cm_hir_dump_named(stream, context,
                    &supertrait->trait_type);
                fprintf(stream, " equalities=%u span=",
                    (unsigned int)supertrait->equality_count);
                cm_hir_dump_span(stream, supertrait->span);
                fputc('\n', stream);
                {
                    uint32_t equality_index;

                    for (equality_index = 0u;
                         equality_index < supertrait->equality_count;
                         ++equality_index) {
                        const CmHirAssociatedTypeEquality *equality;

                        equality = &supertrait->equalities[equality_index];
                        fprintf(stream,
                            "supertrait-associated-type-equality item#%u "
                            "supertrait=%u index=%u associated=",
                            (unsigned int)(index + 1u),
                            (unsigned int)supertrait_index,
                            (unsigned int)equality_index);
                        cm_hir_dump_def(stream, equality->associated_type);
                        fprintf(stream, " value=ty#%u span=",
                            (unsigned int)equality->value);
                        cm_hir_dump_span(stream, equality->span);
                        fputc('\n', stream);
                    }
                }
            }
        }
        if (item->kind == CM_HIR_ITEM_TRAIT_ALIAS) {
            uint32_t bound_index;

            for (bound_index = 0u;
                 bound_index < item->data.trait_alias_item.bound_count;
                 ++bound_index) {
                const CmHirTraitAliasBound *bound;

                bound = &item->data.trait_alias_item.bounds[bound_index];
                if (bound->kind == CM_HIR_TRAIT_ALIAS_BOUND_LIFETIME) {
                    fprintf(stream,
                        "trait-alias-bound item#%u index=%u "
                        "kind=lifetime lifetime=",
                        (unsigned int)(index + 1u),
                        (unsigned int)bound_index);
                    cm_hir_dump_region(stream, context,
                        &bound->data.lifetime);
                    fputs(" span=", stream);
                    cm_hir_dump_span(stream, bound->span);
                    fputc('\n', stream);
                } else {
                    const CmHirSupertrait *trait_bound;
                    uint32_t equality_index;

                    trait_bound = &bound->data.trait_bound;
                    fprintf(stream,
                        "trait-alias-bound item#%u index=%u kind=trait "
                        "modifier=%s trait=",
                        (unsigned int)(index + 1u),
                        (unsigned int)bound_index,
                        cm_hir_supertrait_modifier_name(
                            trait_bound->modifier));
                    cm_hir_dump_named(stream, context,
                        &trait_bound->trait_type);
                    fprintf(stream, " equalities=%u span=",
                        (unsigned int)trait_bound->equality_count);
                    cm_hir_dump_span(stream, bound->span);
                    fputc('\n', stream);
                    for (equality_index = 0u;
                         equality_index < trait_bound->equality_count;
                         ++equality_index) {
                        const CmHirAssociatedTypeEquality *equality;

                        equality = &trait_bound->equalities[equality_index];
                        fprintf(stream,
                            "trait-alias-associated-type-equality "
                            "item#%u bound=%u index=%u associated=",
                            (unsigned int)(index + 1u),
                            (unsigned int)bound_index,
                            (unsigned int)equality_index);
                        cm_hir_dump_def(stream,
                            equality->associated_type);
                        fprintf(stream, " value=ty#%u span=",
                            (unsigned int)equality->value);
                        cm_hir_dump_span(stream, equality->span);
                        fputc('\n', stream);
                    }
                }
            }
        }
        if (item->kind == CM_HIR_ITEM_TYPE_ALIAS) {
            uint32_t bound_index;

            for (bound_index = 0u;
                 bound_index < item->data.type_alias_item.bound_count;
                 ++bound_index) {
                const CmHirAssociatedTypeBound *bound;
                uint32_t equality_index;

                bound = &item->data.type_alias_item.bounds[bound_index];
                fprintf(stream,
                    "associated-type-bound item#%u index=%u modifier=%s "
                    "trait=",
                    (unsigned int)(index + 1u), (unsigned int)bound_index,
                    cm_hir_associated_bound_modifier_name(bound->modifier));
                cm_hir_dump_named(stream, context, &bound->trait_type);
                fprintf(stream, " equalities=%u span=",
                    (unsigned int)bound->equality_count);
                cm_hir_dump_span(stream, bound->span);
                fputc('\n', stream);
                for (equality_index = 0u;
                     equality_index < bound->equality_count;
                     ++equality_index) {
                    const CmHirAssociatedTypeEquality *equality;

                    equality = &bound->equalities[equality_index];
                    fprintf(stream,
                        "associated-type-equality item#%u bound=%u "
                        "index=%u associated=",
                        (unsigned int)(index + 1u),
                        (unsigned int)bound_index,
                        (unsigned int)equality_index);
                    cm_hir_dump_def(stream, equality->associated_type);
                    fprintf(stream, " value=ty#%u span=",
                        (unsigned int)equality->value);
                    cm_hir_dump_span(stream, equality->span);
                    fputc('\n', stream);
                }
            }
        }
        if (item->kind == CM_HIR_ITEM_FUNCTION) {
            const CmHirFunctionSignature *signature;
            uint32_t parameter_index;

            signature = &item->data.function_item.signature;
            for (parameter_index = 0u;
                 parameter_index < signature->parameter_count;
                 ++parameter_index) {
                const CmHirFunctionParameter *parameter;

                parameter = &signature->parameters[parameter_index];
                fprintf(stream,
                    "function-param item#%u index=%u binding=%s name=",
                    (unsigned int)(index + 1u),
                    (unsigned int)parameter_index,
                    cm_hir_binding_name(parameter->binding_kind));
                if (parameter->binding_kind != CM_HIR_BINDING_NAMED) {
                    fputs("none", stream);
                } else {
                    cm_hir_dump_string(stream, context, parameter->name);
                }
                fprintf(stream, " mode=%s type=ty#%u span=",
                    cm_hir_parameter_binding_mode_name(
                        parameter->binding_mode),
                    (unsigned int)parameter->type);
                cm_hir_dump_span(stream, parameter->span);
                fputc('\n', stream);
                if (parameter->binding_kind
                        == CM_HIR_BINDING_TUPLE_PATTERN) {
                    const CmHirType *tuple_type;
                    uint32_t binding_count;
                    uint32_t binding_index;

                    tuple_type = cm_hir_get_type(context, parameter->type);
                    binding_count = tuple_type != NULL
                            && tuple_type->kind == CM_HIR_TYPE_TUPLE_KIND
                            && tuple_type->data.tuple_type.element_count
                                <= CM_HIR_TUPLE_PARAMETER_BINDING_COUNT
                        ? tuple_type->data.tuple_type.element_count : 0u;
                    for (binding_index = 0u;
                         binding_index < binding_count;
                         ++binding_index) {
                        fprintf(stream,
                            "function-param-binding item#%u parameter=%u "
                            "index=%u name=",
                            (unsigned int)(index + 1u),
                            (unsigned int)parameter_index,
                            (unsigned int)binding_index);
                        cm_hir_dump_string(stream, context,
                            parameter->tuple_bindings[binding_index].name);
                        fputs(" span=", stream);
                        cm_hir_dump_span(stream,
                            parameter->tuple_bindings[binding_index].span);
                        fputc('\n', stream);
                    }
                } else if (parameter->binding_kind
                        == CM_HIR_BINDING_NEWTYPE_PATTERN) {
                    fprintf(stream,
                        "function-param-binding item#%u parameter=%u "
                        "index=0 name=",
                        (unsigned int)(index + 1u),
                        (unsigned int)parameter_index);
                    cm_hir_dump_string(stream, context,
                        parameter->newtype_binding.name);
                    fputs(" span=", stream);
                    cm_hir_dump_span(stream,
                        parameter->newtype_binding.span);
                    fputc('\n', stream);
                }
            }
        }
    }
    return ferror(stream) ? -1 : 0;
}
