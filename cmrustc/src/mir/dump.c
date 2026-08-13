#include "cm/mir/model.h"

static const char *cm_mir_local_kind_name(CmMirLocalKind kind)
{
    switch (kind) {
    case CM_MIR_LOCAL_RETURN: return "return";
    case CM_MIR_LOCAL_ARGUMENT: return "argument";
    case CM_MIR_LOCAL_USER: return "user";
    case CM_MIR_LOCAL_TEMPORARY: return "temporary";
    }
    return NULL;
}

static int cm_mir_dump_instance(FILE *stream, const CmMirInstance *instance)
{
    uint32_t index;

    if (fprintf(stream, "%u:%u/body=%u:%u",
            (unsigned int)instance->definition.crate_id,
            (unsigned int)instance->definition.index,
            (unsigned int)instance->body_definition.crate_id,
            (unsigned int)instance->body_definition.index) < 0) {
        return -1;
    }
    if (instance->substitution_count == 0u) return 0;
    if (fputc('<', stream) == EOF) return -1;
    for (index = 0u; index < instance->substitution_count; ++index) {
        if (index != 0u && fputs(",", stream) == EOF) return -1;
        if (fprintf(stream, "ty#%u",
                (unsigned int)instance->substitutions[index]) < 0) {
            return -1;
        }
    }
    return fputc('>', stream) == EOF ? -1 : 0;
}

static int cm_mir_dump_place(FILE *stream, const CmMirPlace *place)
{
    uint32_t index;

    if (fprintf(stream, "place(_%u", (unsigned int)place->base) < 0) {
        return -1;
    }
    for (index = 0u; index < place->projection_count; ++index) {
        const CmMirPlaceProjection *projection;

        projection = &place->projections[index];
        if (projection->kind == CM_MIR_PROJECTION_DEREFERENCE) {
            if (!cm_hir_def_id_is_none(projection->definition)
                || projection->field_index != 0u
                || fputs(".deref", stream) == EOF) return -1;
        } else if (projection->kind == CM_MIR_PROJECTION_FIELD
            && fprintf(stream, ".field(%u:%u#%u)",
                (unsigned int)projection->definition.crate_id,
                (unsigned int)projection->definition.index,
                (unsigned int)projection->field_index) < 0) {
            return -1;
        } else if (projection->kind != CM_MIR_PROJECTION_FIELD) {
            return -1;
        }
    }
    return fprintf(stream, ",type=ty#%u,span=%u:%u..%u)",
        (unsigned int)place->type, (unsigned int)place->span.source,
        (unsigned int)place->span.start,
        (unsigned int)place->span.end) < 0 ? -1 : 0;
}

static int cm_mir_dump_operand(FILE *stream, const CmMirOperand *operand,
    int call_argument)
{
    if (operand->kind == CM_MIR_OPERAND_MOVE) {
        if (call_argument) {
            return fprintf(stream, "move _%u:ty#%u",
                (unsigned int)operand->data.local,
                (unsigned int)operand->type) < 0 ? -1 : 0;
        }
        return fprintf(stream, "move _%u type=ty#%u",
            (unsigned int)operand->data.local,
            (unsigned int)operand->type) < 0 ? -1 : 0;
    }
    if (operand->kind == CM_MIR_OPERAND_MOVE_PLACE
        || operand->kind == CM_MIR_OPERAND_COPY_PLACE) {
        const char *operation;

        operation = operand->kind == CM_MIR_OPERAND_MOVE_PLACE
            ? "move" : "copy";
        if (fprintf(stream, "%s(", operation) < 0
            || cm_mir_dump_place(stream, &operand->data.place) != 0
            || fprintf(stream, "):ty#%u", (unsigned int)operand->type) < 0) {
            return -1;
        }
        return 0;
    }
    if (operand->kind == CM_MIR_CONSTANT_I32) {
        if (call_argument) {
            return fprintf(stream, "const-i32(%ld):ty#%u",
                (long)operand->data.i32_value,
                (unsigned int)operand->type) < 0 ? -1 : 0;
        }
        return fprintf(stream, "const-i32(%ld) type=ty#%u",
            (long)operand->data.i32_value,
            (unsigned int)operand->type) < 0 ? -1 : 0;
    }
    if (operand->kind == CM_MIR_CONSTANT_U32) {
        if (call_argument) {
            return fprintf(stream, "const-u32(%lu):ty#%u",
                (unsigned long)operand->data.u32_value,
                (unsigned int)operand->type) < 0 ? -1 : 0;
        }
        return fprintf(stream, "const-u32(%lu) type=ty#%u",
            (unsigned long)operand->data.u32_value,
            (unsigned int)operand->type) < 0 ? -1 : 0;
    }
    if (operand->kind == CM_MIR_CONSTANT_USIZE) {
        if (call_argument) {
            return fprintf(stream, "const-usize(%llu):ty#%u",
                (unsigned long long)operand->data.usize_value,
                (unsigned int)operand->type) < 0 ? -1 : 0;
        }
        return fprintf(stream, "const-usize(%llu) type=ty#%u",
            (unsigned long long)operand->data.usize_value,
            (unsigned int)operand->type) < 0 ? -1 : 0;
    }
    return -1;
}

static int cm_mir_dump_rvalue(FILE *stream, const CmMirRvalue *rvalue)
{
    if (rvalue->kind == CM_MIR_RVALUE_USE) {
        if (fputs("use(", stream) == EOF
            || cm_mir_dump_operand(stream, &rvalue->data.use, 1) != 0
            || fprintf(stream, ") type=ty#%u",
                (unsigned int)rvalue->type) < 0) {
            return -1;
        }
        return 0;
    }
    if (rvalue->kind == CM_MIR_RVALUE_BINARY
        && (rvalue->data.binary.operator_kind == CM_MIR_BINARY_ADD
            || rvalue->data.binary.operator_kind
                == CM_MIR_BINARY_SUBTRACT)) {
        if (fprintf(stream, "binary(%s,",
                rvalue->data.binary.operator_kind == CM_MIR_BINARY_ADD
                    ? "add" : "subtract") < 0
            || cm_mir_dump_operand(stream, &rvalue->data.binary.left, 1)
                != 0
            || fputc(',', stream) == EOF
            || cm_mir_dump_operand(stream, &rvalue->data.binary.right, 1)
                != 0
            || fprintf(stream, ") type=ty#%u",
                (unsigned int)rvalue->type) < 0) {
            return -1;
        }
        return 0;
    }
    if (rvalue->kind == CM_MIR_RVALUE_EQUAL) {
        if (fputs("equal(", stream) == EOF
            || cm_mir_dump_operand(stream, &rvalue->data.equal.left, 1)
                != 0
            || fputc(',', stream) == EOF
            || cm_mir_dump_operand(stream, &rvalue->data.equal.right, 1)
                != 0
            || fprintf(stream, ") type=ty#%u",
                (unsigned int)rvalue->type) < 0) {
            return -1;
        }
        return 0;
    }
    if (rvalue->kind == CM_MIR_RVALUE_LESS) {
        if (fputs("less(", stream) == EOF
            || cm_mir_dump_operand(stream, &rvalue->data.less.left, 1)
                != 0
            || fputc(',', stream) == EOF
            || cm_mir_dump_operand(stream, &rvalue->data.less.right, 1)
                != 0
            || fprintf(stream, ") type=ty#%u",
                (unsigned int)rvalue->type) < 0) {
            return -1;
        }
        return 0;
    }
    if (rvalue->kind == CM_MIR_RVALUE_BORROW) {
        if (rvalue->data.borrow.kind != CM_MIR_BORROW_SHARED
            || fputs("borrow(shared,", stream) == EOF
            || cm_mir_dump_place(stream, &rvalue->data.borrow.source) != 0
            || fprintf(stream, ") type=ty#%u span=%u:%u..%u",
                (unsigned int)rvalue->type,
                (unsigned int)rvalue->span.source,
                (unsigned int)rvalue->span.start,
                (unsigned int)rvalue->span.end) < 0) {
            return -1;
        }
        return 0;
    }
    if (rvalue->kind == CM_MIR_RVALUE_AGGREGATE) {
        uint32_t index;

        if (fprintf(stream, "aggregate(%u:%u,[",
                (unsigned int)rvalue->data.aggregate.definition.crate_id,
                (unsigned int)rvalue->data.aggregate.definition.index) < 0) {
            return -1;
        }
        for (index = 0u; index < rvalue->data.aggregate.field_count;
             ++index) {
            const CmMirAggregateField *field;

            field = &rvalue->data.aggregate.fields[index];
            if ((index != 0u && fputc(',', stream) == EOF)
                || fprintf(stream, "field(index=%u,value=",
                    (unsigned int)field->field_index) < 0
                || cm_mir_dump_operand(stream, &field->value, 1) != 0
                || fputc(')', stream) == EOF) {
                return -1;
            }
        }
        return fprintf(stream, "]) type=ty#%u span=%u:%u..%u",
            (unsigned int)rvalue->type,
            (unsigned int)rvalue->span.source,
            (unsigned int)rvalue->span.start,
            (unsigned int)rvalue->span.end) < 0 ? -1 : 0;
    }
    return -1;
}

int cm_mir_dump(FILE *stream, const CmMirContext *context)
{
    size_t body_index;

    if (stream == NULL || context == NULL) return -1;
    if (fprintf(stream, "mir-v9 pointer-bits=%u\n",
            cm_mir_context_pointer_bits(context)) < 0) {
        return -1;
    }
    for (body_index = 0u; body_index < cm_mir_body_count(context);
         ++body_index) {
        CmMirBodyId body_id;
        const CmMirBody *body;
        int exact;
        uint32_t local_index;
        uint32_t block_index;

        body_id = (CmMirBodyId)(body_index + 1u);
        body = cm_mir_get_body(context, body_id);
        if (body == NULL) return -1;
        exact = !cm_hir_def_id_is_none(body->instance.definition);
        if (exact) {
            if (fprintf(stream, "body#%u instance=",
                    (unsigned int)body_id) < 0
                || cm_mir_dump_instance(stream, &body->instance) != 0
                || fprintf(stream,
                    " source-body#%u locals=%u blocks=%u\n",
                    (unsigned int)body->source_body,
                    (unsigned int)body->local_count,
                    (unsigned int)body->basic_block_count) < 0) {
                return -1;
            }
        } else if (fprintf(stream,
                "body#%u owner=%u:%u source-body#%u locals=%u blocks=%u\n",
                (unsigned int)body_id, (unsigned int)body->owner.crate_id,
                (unsigned int)body->owner.index,
                (unsigned int)body->source_body,
                (unsigned int)body->local_count,
                (unsigned int)body->basic_block_count) < 0) {
            return -1;
        }
        for (local_index = 0u; local_index < body->local_count;
             ++local_index) {
            const CmMirLocal *local;
            const char *kind;

            local = &body->locals[local_index];
            kind = cm_mir_local_kind_name(local->kind);
            if (kind == NULL
                || fprintf(stream,
                    "local body#%u _%u kind=%s type=ty#%u\n",
                    (unsigned int)body_id, (unsigned int)local_index, kind,
                    (unsigned int)local->type) < 0) {
                return -1;
            }
        }
        for (block_index = 0u; block_index < body->basic_block_count;
             ++block_index) {
            const CmMirBasicBlock *block;
            uint32_t statement_index;

            block = &body->basic_blocks[block_index];
            if (!exact
                && fprintf(stream, "block body#%u bb%u statements=%u\n",
                    (unsigned int)body_id, (unsigned int)block_index,
                    (unsigned int)block->statement_count) < 0) {
                return -1;
            }
            for (statement_index = 0u;
                 statement_index < block->statement_count;
                 ++statement_index) {
                const CmMirStatement *statement;

                statement = &block->statements[statement_index];
                if (statement->kind != CM_MIR_STATEMENT_ASSIGN
                    || fprintf(stream,
                        "statement body#%u bb%u[%u] assign ",
                        (unsigned int)body_id, (unsigned int)block_index,
                        (unsigned int)statement_index) < 0
                    || (statement->data.assign.destination_place.type
                            != CM_HIR_TYPE_NONE
                        ? cm_mir_dump_place(stream,
                            &statement->data.assign.destination_place)
                        : fprintf(stream, "_%u",
                            (unsigned int)statement->data.assign.destination))
                        < 0
                    || fputs(" = ", stream) == EOF
                    || cm_mir_dump_rvalue(stream,
                        &statement->data.assign.value) != 0
                    || fputc('\n', stream) == EOF) {
                    return -1;
                }
            }
            if (block->terminator.kind == CM_MIR_TERMINATOR_RETURN) {
                if (fprintf(stream, "terminator body#%u bb%u return\n",
                        (unsigned int)body_id,
                        (unsigned int)block_index) < 0) {
                    return -1;
                }
            } else if (block->terminator.kind == CM_MIR_TERMINATOR_CALL) {
                const CmMirTerminator *terminator;
                uint32_t argument_index;

                terminator = &block->terminator;
                if (fprintf(stream,
                        "terminator body#%u bb%u call destination=",
                        (unsigned int)body_id,
                        (unsigned int)block_index) < 0
                    || (terminator->data.call.destination_place.type
                            != CM_HIR_TYPE_NONE
                        ? cm_mir_dump_place(stream,
                            &terminator->data.call.destination_place)
                        : fprintf(stream, "_%u",
                            (unsigned int)terminator->data.call.destination))
                        < 0
                    || fprintf(stream, " callee=body#%u instance=",
                        (unsigned int)terminator->data.call.callee_instance)
                        < 0
                    || cm_mir_dump_instance(stream,
                        &terminator->data.call.callee) != 0
                    || fputs(" args=[", stream) == EOF) {
                    return -1;
                }
                for (argument_index = 0u;
                     argument_index < terminator->data.call.argument_count;
                     ++argument_index) {
                    if (argument_index != 0u
                        && fputs(",", stream) == EOF) {
                        return -1;
                    }
                    if (cm_mir_dump_operand(stream,
                            &terminator->data.call.arguments[argument_index],
                            1) != 0) {
                        return -1;
                    }
                }
                if (fprintf(stream, "] target=bb%u\n",
                        (unsigned int)terminator->data.call.target) < 0) {
                    return -1;
                }
            } else if (block->terminator.kind == CM_MIR_TERMINATOR_GOTO) {
                if (fprintf(stream, "terminator body#%u bb%u goto bb%u\n",
                        (unsigned int)body_id,
                        (unsigned int)block_index,
                        (unsigned int)block->terminator.data.goto_block
                            .target) < 0) {
                    return -1;
                }
            } else if (block->terminator.kind
                    == CM_MIR_TERMINATOR_SWITCH_BOOL) {
                if (fprintf(stream,
                        "terminator body#%u bb%u switch-bool ",
                        (unsigned int)body_id,
                        (unsigned int)block_index) < 0
                    || cm_mir_dump_operand(stream,
                        &block->terminator.data.switch_bool.condition, 0)
                        != 0
                    || fprintf(stream, " true=bb%u false=bb%u\n",
                        (unsigned int)block->terminator.data.switch_bool
                            .true_target,
                        (unsigned int)block->terminator.data.switch_bool
                            .false_target) < 0) {
                    return -1;
                }
            } else {
                return -1;
            }
        }
    }
    return ferror(stream) ? -1 : 0;
}
