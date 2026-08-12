# Parsed AST differential harness

`run_ast_differential.sh` compares `cmrustc` with upstream mrustc at the
parsed-AST boundary.  It does not use expanded or lowered output.  The
developer-only C++ adapter calls `Parse_Crate` and walks `AST::Crate` before
macro expansion; the C adapter calls `cm_parse_crate` and walks `CmAst`.

Run it from the repository root with developer compilers and zlib available:

```sh
nix shell nixpkgs#gcc nixpkgs#zlib nixpkgs#diffutils -c sh -lc \
  'make -C cmrustc check-ast-differential CC=gcc CXX=g++ \
    ORACLE_LDLIBS="-L$(nix eval --raw nixpkgs#zlib)/lib -lz -pthread"'
```

The script builds both adapters in a temporary directory.  The C++ oracle
remains outside the trusted bootstrap; `check-all` compiles the C adapter so
strict switch warnings catch future AST enum drift.  The developer-only
`check-ast-differential` target builds the C++ oracle and runs the comparisons.
It hashes `bin/mrustc`, `bin/mrustc.a`, and `bin/common_lib.a` before and after
every run, compares the hashes, and also invokes the actual
`bin/mrustc -Z stop-after=parse` executable on every fixture.

## Comparison classes

- **Exact schema match** uses an ordered, byte-for-byte facts stream.  It
  retains item nesting/order, item names and visibility, generic arity,
  aggregate and variant forms, field names/types, function flags/ABI,
  parameter outer-pattern kinds and types, return types, body presence, and
  per-body expression/pattern inventories.
- **Normalized semantic match** aggregates item, type, expression, pattern,
  generic-parameter, and function-flag counts across the crate.  It is meant
  to survive harmless representation choices while detecting lost items,
  signatures, bodies, or syntax classes.
- **Expected representation difference** requires the exact streams to
  differ, then requires their semantic streams to match.  The current fixture
  captures a known parsed-AST choice: upstream synthesizes receiver types such
  as `&Self` and implicit `()` returns and marks trait members public, whereas
  `cmrustc` records `is_self` without synthesizing those types and retains
  inherited visibility.

The fixtures cover all currently represented item families (including unions),
references, pointers, tuples, slices, arrays, function, projection, `impl
Trait`, `dyn Trait`, and macro types, generic and where-clause syntax,
associated trait/impl items, and bodies containing lets and let chains,
assignment, binary expressions, qualified paths, calls, method/named/tuple
field access, indexing, casts, postfix `?`, try blocks, raw references, range
expressions, `if let`, `for`, `while`, loops, flow expressions, closures,
arrays, tuples, `match`, range patterns, reference patterns, and rest-bearing
struct/tuple/slice patterns.

## Deliberate normalization

The common schema is a structural differential signal, not a claim that the
two in-memory AST layouts are identical.  It deliberately discards:

- source spans, comments, whitespace, and attribute payloads;
- generic argument payloads, parameter bounds, supertrait text, and the exact
  spelling/location of `where` clauses (their items and generic arity remain);
- array-length and discriminant values;
- expression-tree topology, identifiers, literal values, and operator spelling
  after expression nodes have been assigned to a syntax class;
- pattern binding names and mutability after pattern classification;
- inferred/missing types and implicit/empty unit types in semantic mode;
- item names, visibility, order, and nesting in semantic mode.

Paths and full type shapes are retained in exact mode except for generic path
arguments.  Expression and pattern support is compared as parsed node-kind
inventories; this is stronger than body-presence testing but intentionally does
not claim expression-tree identity.

External `mod name;` fixtures are avoided because upstream correctly performs
filesystem module loading.  Expansion-sensitive macros and attributes are
also outside this parsed-AST comparison.
