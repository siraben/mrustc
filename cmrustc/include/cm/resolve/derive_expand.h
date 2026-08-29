#ifndef CM_RESOLVE_DERIVE_EXPAND_H
#define CM_RESOLVE_DERIVE_EXPAND_H

#include "cm/syntax/ast.h"
#include "cm/syntax/parser.h"

/*
 * Builtin derives (M9): every struct/enum item carrying
 * `#[derive(Debug | Clone | Copy | PartialEq | Eq | Default)]` gets the
 * corresponding `impl` synthesized as source text, parsed into `ast`, and
 * appended to the item's containing module.  Runs once right after a
 * unit is parsed.  Returns the number of items appended (0 on none).
 */
/* `core_reachable`: the crate can name `::core` (a dependency provides it);
 * an `extern crate .. as core` alias in the unit also counts. */
size_t cm_derive_expand(CmAst *ast, enum cm_edition edition,
    int core_reachable);

/* Whether the unit declares `extern crate .. as core` (core itself has
 * `extern crate self as core`). */
int cm_derive_unit_aliases_core(const CmAst *ast);

#endif
