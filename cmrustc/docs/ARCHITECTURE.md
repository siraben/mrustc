# Architecture

## Scope and evidence

The C++ oracle contains about 153,000 compiler lines and another 8,500 lines
of bootstrap tooling. Its largest areas are type/trait inference, MIR, HIR,
and C code generation. It also relies heavily on STL containers, templates,
virtual dispatch, exceptions, lambdas, and pointer ownership. Translating
those mechanisms directly would preserve the oracle's coupling while making
the C port harder to review.

The new compiler keeps the proven pass ordering but owns a smaller data model:

```text
source
  -> tokens and token trees
  -> parsed AST
  -> cfg, attributes, macros, proc macros
  -> resolved HIR
  -> inferred/selected types and impls
  -> closure/generator/vtable lowering
  -> MIR
  -> reachable monomorphized MIR
  -> TCC-dialect C
  -> object or executable
```

Cross-crate metadata and build orchestration are first-class parts of the
compiler, not deferred packaging work.

The 2026-08-23 audit measured 129,227 production C/header lines and 101,804
C/header/shell test and tool lines against 154,268 lines in the original
compiler.
The implemented weight is concentrated in syntax, macros, resolution, and
declaration HIR. A narrow no-core path genuinely lowers exact typed bodies to
MIR, computes reachable direct instances, emits C99, and executes GCC/TinyCC
artifacts; it is not scaffolding. The original's general inference/trait
solver, full MIR and monomorphization, runtime/codegen breadth, semantic
cross-crate libraries, and build orchestration remain the dominant missing
weight. The current C99 model is a credible and more auditable foundation than
the abandoned Haskell linkability experiment, but declaration coverage is not
a proxy for bootstrap depth. After the target-configured core declaration-HIR
gate closes, the critical path is body inference/trait obligations and one
generic value/trait implementation carried across a crate boundary through
metadata, MIR, monomorphization, linking, and execution.

The current target-configured Rust 1.90 core probe resolves a real inherited
implicit prelude across all 363 sources and 451 modules and reports zero graph,
import, and HIR errors. It has crossed positional and associated-equality
supertraits, including `ops::Shr<u32, Output = Self>`, and structural
`Self: 'static` lifetime supertraits. Trait aliases retain their ordered trait
and lifetime RHS bounds, including generic arguments and associated
equalities, and may be referenced by predicates and ordinary supertraits while
mixed cycles and impl headers fail closed. Authenticated auto-trait identity
and safe negative impl polarity are now structural HIR rather than inferred
from names; only itemless negative impls of auto traits are admitted, and a
const negative impl must target a compiler-authenticated const trait. Duplicate
or positive/negative overlap rejects transactionally. Structural
trait type defaults substitute prior lifetime/type arguments and `Self`, and
explicit trait lifetime arguments retain authenticated early- or late-bound
identity. Associated-type trait bounds now retain positional lifetime/type
arguments, and lifetime bounds lower to outlives obligations on the exact
authenticated `<Self as Trait>::Associated` projection. Trait defaults that do
not mention their bounded `Self` remain separate from associated equalities.
Trait defaults such as `PartialEq<Rhs = Self>` now substitute the exact
authenticated associated projection rather than the enclosing trait's
`Self`, clearing `DiscriminantKind::Discriminant` in `marker.rs`. Whole-core
declaration HIR is complete; the active frontier is M6-06's consumable core
metadata and `.rlib`. This is declaration coverage, not evidence that general
trait obligations or method bodies are executable.

The canonical metadata producer is likewise a real parsed-source vertical:
source graph, imports, HIR lowering, public artifact capture, deterministic
encoding, fresh-process decode, and runtime-ID remapping. Its boundary remains
declaration-only raw `cmhir` bytes. The private driver switches
`--emit-cmhir SOURCE --crate-name NAME -o FILE` and repeated
`--extern-cmhir NAME FILE` provide real producer and dependent-load paths with
atomic publication. The v2.3 declaration format carries predicate-free public
free functions (including lifetime/type/const parameters), public consts and
statics, const generic parameters, and literal/parameter const uses in named
arguments and array lengths. Version 2.4 appends canonical `NREF` and sparse
`PRED` sections for a bounded scope-free public-function predicate shape.
Version 2.5 adds a stable modifier byte and preserves REQUIRED,
CONST_IF_CONST, and CONST without treating them as const-trait eligibility.
Version 2.6 admits an opaque trait-alias NREF as the direct target of a PRED
record, but forbids direct alias equalities and transports no alias expansion.
Referenced traits and associated types remain opaque RESERVED identities with
authenticated names, owners, generic schemas, declaring parents, and explicit
associated-availability witnesses; they create no item or namespace binding.
Predicate-owned late-bound input regions are accepted only beneath their
authenticating binder. Decoder preflight checks canonical ordering, aggregate
resource limits, type reachability and nesting, generic provenance, and all
cross-section references before reserving runtime definitions. Exact v2.6
decode falls back through exact v2.5, v2.4, and v2.3 only for an unsupported version;
malformed current-version payloads never fall through. None of these formats
yet carries the complete core trait/alias/impl, body, instantiation,
dependency-archive, or link-input surface, and none is yet a consumable core
`.rmeta` or `.rlib`.

The downstream-safe successor is specified in `docs/METADATA_V3.md`. It uses
an exact capability manifest and distinct complete declaration/positive
semantic profiles so omitted traits, associated declarations, impls, macros,
bodies, and link inputs cannot be mistaken for an empty or complete family.
V3 is a new major because its unified declaration identities, trait namespace
targets, and completeness semantics are incompatible with the opaque v2
boundary.

The preceding Haskell `hrustc` experiment is useful negative evidence. Its
last session reached a rustc-like binary that accepted `hello.rs` and exited
successfully, but emitted no artifact; work stopped at output-path handling
around commit `d34a6675`/task 121. Zero-valued semantic placeholders, no-op
operations, and identity conversions had already made the apparent surface
coverage unreliable. The C rewrite does not count a successful exit as a
compiler milestone: every vertical gate requires a nonempty artifact and,
where applicable, a linked executable with observed behavior.

## Dependency rule

Dependencies flow down this list only:

1. `base`: allocation, arenas, strings, vectors/maps, interning, diagnostics,
   files, processes, target facts.
2. `syntax`: source files, spans, tokens, token trees, AST, parser.
3. `macro`: cfg and attributes, `macro_rules!`, builtins, proc-macro protocol.
4. `resolve`: crates, modules, namespaces, imports, absolute paths.
5. `hir`: canonical semantic schema and cross-crate metadata.
6. `types`: inference, traits, associated types, coercions, const evaluation.
7. `lower`: closures, async/generators, vtables, erased types, HIR-to-MIR.
8. `mir`: MIR schema, cleanup, validation, optional optimization.
9. `mono`: reachability, substitution, layout, ABI, monomorphization.
10. `backend_tcc`: C emission, portability runtime, compile/link driver.
11. `hcargo`: manifests, crate graph, overrides, build scripts, job runner.

Schemas are versioned headers owned by their defining layer. A pass may store
stable integer IDs but must not expose pointers into its arena to another
layer. Cycles are resolved through narrow query interfaces rather than header
inclusion.

## C representation

- AST, HIR, MIR, and type nodes use `enum` tags plus `union` payloads.
- Repeated node definitions use checked X-macro schemas. Generated files are
  reproducible and committed only when needed by the stage0 build.
- Collections are typed wrappers over a small vector and open-addressed map.
- Compiler-lifetime strings are interned; source slices retain file IDs and
  offsets instead of allocating substrings.
- AST, HIR, MIR, inference, and scratch data use separate bump arenas.
- Cross-layer references are integer IDs. ID zero is invalid unless the type
  explicitly defines it as `none`.
- Walkers are switch-based functions. Callbacks are function pointer plus
  context pointer pairs.
- Recoverable failures use explicit result structs. Fatal compiler errors use
  one driver-owned `setjmp` boundary, never nested exception emulation.

## Portability dialect

The compiler source uses a documented TinyCC 0.9.27-compatible C99 subset:

- no variable-length arrays, `_Generic`, C11 atomics, compiler statement
  expressions, or nested functions;
- C99 block declarations, designated initializers, compound literals,
  `long long`, `_Bool`, and variadic macros are allowed after probes;
- no dependence on `__int128`, TLS, weak symbols, or constructor attributes;
- fixed-width types supplied by a checked compatibility header;
- no mandatory zlib in the stage0 compiler; metadata starts uncompressed;
- no shell-specific build logic in the compiler executable.

Both `tcc -Wall -Werror` and a stricter development compiler must build every
translation unit. The generated Rust C has a separate backend contract. TCC
must compile no-core and selected runtime probes; the full Rust graph may use
the later source-built GCC while retaining source-only provenance.

## Semantic shortcuts

Safe to omit initially:

- borrow checking and lifetime rejection;
- exhaustive validation and MIR optimization;
- lints, privacy diagnostics, coherence diagnostics for invalid programs;
- recovery after the first compiler error;
- unused target triples and ABIs;
- metadata compression and stable diagnostic formatting.

Not safe to omit:

- enough inference and trait selection to identify concrete calls and layouts;
- moves, destruction, unwind/abort behavior observable by valid code;
- macro expansion and proc-macro execution;
- closures, async/generators, vtables, unsizing, and const evaluation used by
  the bootstrap corpus;
- target ABI, symbol mangling, reachability, and monomorphization;
- cross-crate metadata and native link dependencies.

Conservative memory leakage is permitted during compiler execution and in
generated bootstrap tools only when object lifetime is otherwise ambiguous.
Returning zero, dropping a call, or guessing an impl is never an accepted
fallback.

### First executable body checkpoint

The first executable slice is a vertical implementation rather than a special
success exit in the driver. A source-backed top-level function body is
authenticated against the revision- and owner-latched graph/HIR module map,
enters a typed HIR expression arena through a model-validated state transition,
lowers into separately owned MIR, is validated by the portable C emitter, and
is published by the compile orchestrator. The initial expression schema
contains only suffixed `i32` integers and
statement-free blocks; the initial MIR contains one return local, one constant
assignment, and one return terminator. Unsupported expressions, types, entry
metadata, crate contents, targets, or MIR shapes hard-error.

The legacy literal entry path admits exactly one root module, one body-bearing
entry item, no imports, and no additional expressions or items. This prevents
an active symbol from being silently omitted on that compatibility path.

### First reachable call checkpoint

The next vertical slice admits named parameter reads and the exact call
`identity::<u32>(x)`. HIR call nodes own an ordered substitution list and
argument-expression list and retain the resolved free-function DefId. MIR
instances are keyed by the DefId plus ordered concrete TypeIds, have explicit
argument locals and move operands, and use a resolved call terminator whose
callee instance must already be published. MIR storage is independently
owned and validated against the typed source HIR.

Every public, free `#[no_mangle]` function is an explicit root on this path.
The compiler recursively lowers direct callees before callers, rejects cycles
for now, and emits every and only instance in that root closure. Consequently
a private unreachable unsupported body is ignored and omitted, while an
unsupported exported body rejects the whole compile. Code generation
rechecks that all MIR bodies are root-reachable, emits a real static u32
identity specialization, and emits the exported `probe` as a real call rather
than a constant or invented implementation.

Call arguments may now be recursively nested u32-add trees. HIR owns the full
argument tree; MIR lowers every binary node to a postorder temporary assignment
in the call block, then passes the final temporary to the resolved callee. The
model validates the statements and call operand as one exact tree, and the C
emitter declares the temporaries, executes their assignments in order, and
performs the real call. A plain local argument remains the zero-statement
special case.

The same exact path admits ordinary monomorphic callees with one or two u32
parameters. A call owns and preflights each argument independently, lowers the
argument trees left-to-right through one shared temporary sequence, and retains
the complete ordered operand array in its MIR terminator. Reachability lowers a
zero-substitution callee before its caller. Model validation consumes every
statement, temporary, and operand against the corresponding HIR argument, and
portable C emits the actual ordered one- or two-argument call. The original
one-substitution generic identity instance remains supported by the same path.

Calls may also appear recursively inside another call argument or either side
of an admitted addition. HIR preflights the complete expression tree and adopts
all nested call slices as one transaction; recursive reachability visits every
callee in expression order before lowering the caller. MIR evaluates the tree
left-to-right, ends the current basic block at each call, stores a non-root call
result in the next exact temporary, and resumes in the immediately following
block. The final block either returns a root call directly or finishes the
remaining addition assignments before returning. Model validation replays this
whole HIR tree against the linear CFG, including call keys, destinations,
operands, continuation targets, statement boundaries, and temporary ownership.
The C emitter flattens only that authenticated sequence, so a nested call is
neither reordered nor mistaken for an early function tail.

### First arithmetic checkpoint

The next exact expression slice adds owned, fully typed HIR `u32` literals and
binary nodes. HIR preflights and reserves an entire `+` tree before mutation,
then builds children in source order; wrong suffixes, out-of-range literals,
unresolved locals, non-u32 types, and every other operator reject without
publishing a partial body. Parenthesized forms need no special case because
the parser has already represented their contained expression.

MIR assignments now own explicit `USE` or `BINARY` rvalues. Operands distinguish
signed i32 constants, exact u32 constants, and local moves, so `UINT32_MAX`
never passes through signed storage. The first executable binary shape is an
exact two-parameter public C-ABI function whose return rvalue adds two u32
locals. Generated C retains both arguments and emits an explicit `uint32_t`
cast around their C addition. The unsigned result is therefore modulo 2^32;
the executable gate includes `UINT32_MAX + 1 == 0` and nontrivial wrapping
cases.

Recursive u32-add trees now lower in deterministic left-to-right postorder.
Each non-root binary node receives one u32 temporary local; its assignment is
emitted before the unique parent use, and the root assignment alone targets
the return local. The MIR model replays the HIR tree and requires exact
statement order, operands, types, destinations, and temporary ownership. It
rejects cycles, use-before-definition, duplicate writes, reordered trees, and
unused temporary locals. Generated C declares every temporary and emits each
validated assignment in MIR order, retaining defined wrapping-u32 semantics.

The same typed binary spine now admits u32 subtraction. HIR retains a distinct
operator, MIR validates and dumps the corresponding `SUBTRACT` rvalue, and C
emits an explicit `uint32_t` cast around unsigned subtraction. This preserves
modulo-2^32 release arithmetic without routing through a signed type. The
executable gate covers both ordinary subtraction and underflow; signed
subtraction and every other binary operator remain rejected atomically.

### First value-producing control-flow checkpoint

The first branch slice accepts exact `u32 == u32` only as the condition of a
value-producing `if { ... } else { ... }` whose result is `u32`. MIR retains
equality as a distinct bool-producing rvalue and authenticates one diamond:
the switch block ends in equality, the true and false blocks each assign the
same destination, both go to one empty return join, and no other edge is
accepted. Ordered arithmetic-tree assignments may precede the equality or the
final assignment in either arm, but general CFG emission is still rejected.

Portable C represents this internal bool as `uint8_t`, materializes exactly
`UINT8_C(1)` or `UINT8_C(0)`, and tests it explicitly against zero. Bool never
appears in an emitted function signature. Both arms retain explicit wrapping
`uint32_t` arithmetic, so the executable canary proves equal-path overflow and
unequal-path underflow. `!=`, equality on signed integers, a missing `else`,
and every noncanonical reachable graph reject without replacing or creating
the requested artifact.

### Immutable let checkpoint

The next sequencing slice admits top-level immutable identifier bindings with
an exact written `u32` type, a required initializer, no shadowing, and the
existing literal/local/add/direct-call expression vocabulary. HIR preflights
the complete block before mutation, then transactionally extends the body's
arena-owned local table and interns names under matching rollback marks. The
block owns one flat, source-ordered statement array. Initializer lookup sees
only parameters and prior lets, so self and forward reads fail before
publication.

MIR local order is return, arguments, user locals, then temporaries. Each
initializer lowers directly into its user-local destination, including calls
whose continuation resumes with the following initializer or tail. The model
replays the complete HIR block against every local, assignment, call,
destination, basic block, continuation, and temporary. A public validator
rechecks an already stored monomorphized body without mutation; C emission uses
that semantic authority and separately checks only what the portable-C backend
can represent. Driver reachability visits initializer trees in source order
before the tail and copies its current expression record before recursive
callee lowering can reallocate the HIR vector.

All semantic phases and C formatting complete in memory. Output publication
uses a unique temporary beside the requested path and an atomic rename, so
rejection preserves any previous artifact. Device/inode comparison rejects
hard-link and symlink aliases of the input. Typed local/call/let expressions
use canonical HIR schema `hir-v33`. MIR began at `mir-v1`; user locals,
statement-bearing blocks, flattened aggregate places, and the first exact
conditional diamond, target-width `usize`, and explicit dispatch/body-owner
identity advance the current canonical schema to `mir-v9`.

The generation-bound whole-local-body barrier now reaches a read-only REGIONS
checkpoint after TYPED and MARKED. MARKED atomically records builtin-Copy
value usage for the bounded C expression slice and a NOT_PROMOTED sentinel.
REGIONS recomputes that bounded usage while replaying the complete represented
expression forest, authenticates owner/body,
signature, local, expression-type, and direct-call-substitution roots, and
recursively checks nested type, generic, and const arguments. It admits
`static`, erased, exact item/enclosing-frame early-bound regions, and
nearest-binder function-pointer late-bound regions while rejecting
inference/error/free-or-outer-captured late-bound regions, inference/error
types or consts,
malformed arrays, type cycles, expression DAGs/cycles/orphans, and unresolved
method/qualified/borrow/dereference forms. Success rotates the process-local
capability without changing either HIR generation. This is structural
zero-inference closure for represented bodies, not lifetime equality,
outlives, promotion eligibility, place, or borrow checking. Item predicates,
supertraits, associated bounds, ADT declaration fields, enum discriminants,
and type-position expression bodies are deliberately not REGIONS roots yet;
manifest body owners and enclosing trait/impl items with any predicate or
outlives constraint are therefore rejected fail-closed.

Function-pointer types in `hir-v33` own an ordered lifetime binder. Explicit
and elided input lifetimes use indices local to the nearest function pointer;
nested pointers close independently, with no binder-depth representation or
outer late capture. A derived per-type late-bound requirement is recomputed on
commit from already committed children, ignored on input, and consumed by a
valid local binder. It is validation cache only and is absent from dumps and
semantic identity. HIR equality compares binder arity but not provenance
names. Typeck and in-memory semantic-results storage, canonical instance v2,
semantic metadata v1.1, and declaration metadata v2.x reject nonzero arity
because they cannot preserve the complete region topology.

Canonical `hir-v30` adds stable source-closure identity without claiming
closure semantics. The context owns a one-based closure arena; reservation
deep-copies the complete lexical parameter signature before its nominal
closure type or parameter-read expressions can be built, and body binding is
single-assignment. Closure code remains a nested expression tree owned by its
enclosing item body, with an exact visible-local prefix and closure-owned
parameters rather than synthetic body locals. Finalization, dumps, typed
snapshot/fingerprint checks, rewinds, and semantic-observer counts cover the
arena. Capture absence/class, Copy proofs, invocation, lifetime inference,
expansion, MIR, and codegen are not represented as evidence yet, so every
semantic/executable consumer rejects source-closure nodes fail-closed.

Every represented item body still carries an authenticated `ITEM_SOURCE`
origin: its executable definition, enclosing definition, item backlink, and
transitional lexical owner are the same function, const, or static definition.
This preserves one body per item and current execution behavior. Type-position
and generated origins remain later atomic migrations.

Exact callable instances now keep two authenticated definitions. The selected
callable is the dispatch and symbol identity, while `body_definition` owns the
signature and HIR body that execute. Canonical instance format v2 encodes both
identities and the complete selected impl and substitution domains. Semantic
results, admission, MIR reachability/lowering, and C emission use the body
owner for executable source lookup while retaining dispatch identity in call
edges and symbols. MIR dumps expose both as `dispatch/body` and use `mir-v9`.
For an explicit override, `selected_callable == body_definition` is the impl
method and `declared_trait_callable` is the trait declaration. For an inherited
default, `selected_callable == body_definition == declared_trait_callable` is
the trait method: `selected_impl`, `self_owner`, `self_type`, and their
arguments retain the concrete dispatch identity without inventing a synthetic
method DefId. The executable MIR substitutions instead come from the
trait-owned body domain. Thus two concrete impls may share one HIR body while
retaining unequal canonical identities and distinct digest-derived C symbols.

Execution is currently limited to qualified, receiver-free calls of a
nongeneric default with the closed scalar body subset through concrete local
impls. An explicit bodyful override wins; a bodyless linked override prevents
fallback; multiple linked overrides reject. Dot-method inherited defaults,
calls inside a default body, meaningful `Self`, generic traits, and
source-level blanket inherited defaults remain unsupported.

### Named aggregate expression checkpoint

The parser constructs named struct expressions for explicit fields, shorthand
fields, empty forms, and a final `..base`. Shorthand fields retain both their
source marker and a synthesized one-segment path expression, so ordinary AST
walkers do not need a special missing-value case. Along the ambiguous heads of
`if`, `while`, `for`, and `match`, an unparenthesized top-level struct literal
is disabled across the precedence spine; delimited child expressions, such as
parentheses and call arguments, parse normally. Deterministic AST dumps expose
the update base. `unsafe { ... }` is retained as an explicitly marked block,
and strict or edition keywords that cannot begin a Rust expression path are
not reinterpreted as struct names. A byte-identical copy of Rust 1.90
`library/core/src/future/pending.rs` is a parser gate.

HIR `hir-v15` adds complete aggregate construction for a bound local,
nongeneric, named struct. Each source-ordered field record stores the
authenticated declaration ordinal, typed child expression, and field span.
Insertion validates the ADT DefId, exact field coverage, uniqueness, child
owner and type, source order, and nested spans before allocation, then
deep-copies the field array. Tuple, generic, cross-crate, incomplete, duplicate,
and struct-update constructions are not admitted at this boundary.

Source-backed body lowering admits the same complete local nongeneric named
structs. It resolves direct, qualified, and imported-alias constructor paths
through the exact resolver snapshot used for graph HIR, authenticates the
source declaration against the expected ADT, enforces item and field
visibility, and recursively lowers heterogeneous and nested field values using
their declaration types. A preflight counts every expression, call word, and
field record before mutation. Calls and aggregate field slices then share one
aligned transaction allocation; exactly one expression owns its base, so
rollback and context destruction release it once. Empty named structs require
no payload allocation.

Direct named-field projections retain exact base, field-name, and full-source
spans. Graph lowering materializes the nominal self type of each local
nongeneric named struct, including declarations not otherwise mentioned by a
signature, so a freshly constructed base has an authenticated type identity.
The typed `FIELD` expression stores its base expression, aggregate DefId, and
declaration ordinal; validation independently checks the base ADT, owner,
source containment, result type, module, and crate. Body lowering supports
parameters, fresh reordered aggregates, and nested projection chains while
enforcing field visibility and the exact resolver snapshot. Numeric tuple
projection and C emission remain later checkpoints.

The checked HIR layout layer computes declaration-order size, alignment, and
field offsets for i32/u32 leaves and recursively nested same-crate local
nongeneric named structs. The caller supplies an explicit 32- or 64-bit
address-width model; every alignment and addition is checked against both the
host `size_t` and that model. Recursive, empty, generic, tuple, union,
cross-crate, represented, or otherwise unsupported shapes reject. Results are
transactional: neither the root record nor any field record is published on
failure. This is layout authority for the bounded aggregate slice, not an
exported aggregate ABI.

MIR `mir-v6` admits these same bounded aggregates in direct function bodies.
A place is one local or temporary base, its exact result type and source span,
and at most 16 authenticated aggregate-DefId/declaration-ordinal field steps.
Zero-step local places remain valid. Lowering evaluates aggregate initializer
expressions in source order, then records the complete aggregate rvalue in
declaration order. Nested aggregate expressions use temporaries with their
actual nominal types, and nested field reads flatten into a single projection
path. Scalar i32/u32 reads use `COPY_PLACE`; aggregate-valued reads use
`MOVE_PLACE`.

Stored MIR deep-copies aggregate field arrays, every projection slice, place
operands, and explicit assignment/call destination places. Validation
reauthenticates field owners, ordinals, result types, spans, complete aggregate
coverage, and the entire projection chain before publication. Internal,
monomorphic Rust-ABI calls may carry one or two exact u32 or authenticated
same-crate aggregate operands and must return exact u32. Each argument is
materialized before the call terminator, with aggregate values moved by value.
Aggregate returns, projected writes, generic or cross-crate aggregate calls,
exported aggregate ABI, and represented aggregates remain rejected.

The first aggregate C99 checkpoint discovers the complete nominal type closure
of reachable MIR before output. It validates each type with the checked target
layout query, topologically emits dependency-first ordinal-named C structs, and
adds C99 compile-time proofs for `sizeof`, every `offsetof`, and alignment.
Aggregate rvalues become declaration-ordered designated compound literals;
flattened MIR places become `_local._f0._f1` reads. All layouts, body shapes,
names, and reachable types are authenticated before the first output append.
Internal static signatures use the checked ordinal-named C struct type for
aggregate parameters, and calls pass materialized aggregate locals by value.
Exported signatures remain scalar-u32-only; aggregate returns, aggregate
exports, and projected writes still reject atomically.

The first target-width scalar extension keeps `usize` target-neutral in HIR
but qualifies MIR explicitly. `CmMirContext` has a legacy zero-width state for
u32-only graphs and an immutable 32- or 64-bit pointer width for every graph
that reaches `usize`. MIR stores the full source literal in `uint64_t`, rejects
values outside the selected width before publication, and uses distinct
bool-producing rvalues for u32 equality and usize ordering. Generic
substitution remains u32-only. Checked aggregate layout treats an internal
usize leaf as pointer-sized, but the accepted source call boundary remains an
exact homogeneous scalar usize signature.

The C backend maps `usize` to C99 `uintptr_t`. It never derives Rust target
semantics from host `size_t`; instead each emitted translation unit carries
target-specific `UINTPTR_MAX` and `sizeof(uintptr_t)` assertions and uses
explicit `UINT32_C` or `UINT64_C` constant payloads. Reachability preflight
detects usize recursively through aggregate leaves and rejects a MIR/target
width mismatch before appending output. This preserves the existing width-zero
u32 path while making target-width dependence explicit at the first backend
boundary that needs it.

Bare decimal literal typing is a bounded expected-type pass, not a general
inference engine. HIR body lowering threads an already-exact `i32`, `u32`, or
`usize` expectation through function returns, explicit immutable lets,
aggregate fields, supported branch/operator shapes, and direct-call arguments.
The literal adopts only that exact type; explicit suffixes must agree. A source
without an exact expectation, a conflicting suffix, overflow, hex/underscored
or floating syntax, or any unsupported expected type rejects before HIR
publication. There are no type variables, unification, coercion search, or
fallback-to-i32 semantics in this checkpoint.

### In-memory HIR library snapshot

`CmHirLibraryArtifact` is the first definition-bearing cross-crate boundary.
Construction requires one successful producer graph revision, its exact HIR
module map, and the already-lowered producer crate. It copies a graph-local
public namespace snapshot: public module edges, traits, ADTs, type aliases,
extern types, and same-crate public type/trait/module reexports. Graph source
IDs and AST or resolver pointers never enter the artifact. The graph and map
can be destroyed immediately after construction.

The artifact still references producer DefIds, so producer and consumer crates
must occupy the same live `CmHirContext`; this is an in-memory metadata seam,
not serialization. HIR lowering validates that every borrowed artifact names
that exact context and that extern names are unique before mutation. Local
paths resolve first, artifact paths such as `dep::api::Type` resolve second,
and the legacy external callback is the final fallback. Snapshot traversal is
component-based and follows only copied public module edges. Private paths,
ambiguous entries, wrong-namespace leaves, stale targets, and invalid artifact
arrays fail closed. A producer type alias returns its real producer DefId and
uses the ordinary cross-crate alias normalizer rather than being copied into a
synthetic consumer definition.

Consumer structural `use dep::Type`, `use dep::Trait`, and `use dep::module`
imports are
authenticated only when one exact unresolved non-glob type-namespace leaf
resolves through the artifact's copied public namespace. Authentication binds
the consumer module, exact source-qualified use declaration, local name,
binding kind, and producer DefId. HIR import metadata merges resolver-owned
local bindings with authenticated external bindings in parsed leaf order.
One-segment paths reuse the exact binding check; a longer path may use one
authenticated module alias as its prefix and then traverse only copied public
module edges. Trait lowering binds direct, reexported, imported, and
module-qualified paths to the producer DefId. Generic parameters, safety,
supertraits, and associated declarations are read from the producer's bound
HIR item, so impl headers, bounds, and projections do not fabricate consumer
records. A local collision, competing unresolved leaf or artifact, private
sibling, dependency-rooted glob, or unrelated resolver error fails before HIR
publication. Values, macros, external globs, and transitive reexports of a
third crate remain unauthenticated. The artifact has no standalone encoding
and is not `.rmeta` or `.rlib`; those remain later metadata and
build-orchestration boundaries.

### First process-independent HIR metadata boundary

The first live standalone format is `cmhir-meta-v1`. It is a deliberately
narrow declaration checkpoint, not a claim of core metadata compatibility.
Its semantic payload accepts one producer crate's root-connected module tree,
public extern types, structs, unions, enums and variants, free type aliases,
owner-grouped lifetime/type generics and type defaults, the supported
structural type closure and scalar constants, module/type aliases/reexports,
and builtin primitive bindings. Traits, associated declarations, projections,
impls, bodies, const generics, and unevaluated constants reject encoding as
unsupported HIR.

The envelope uses fixed little-endian integers, an eight-byte magic, an
independent major/minor version, zero feature flags, exact header and payload
lengths, and a payload CRC32. The semantic payload contains exactly six
fixed-order length-framed sections: `CRAT`, `MODS`, `GPAR`, `TYPE`, `ITEM`,
and `NSPC`.
Edition, primitive, and binding tags are explicit numeric wire constants;
C structs are never dumped. Module and item references are one-based
file-local handles, with zero reserved for none. Runtime DefIds,
`CmInternId`s, graph/AST IDs, source IDs, and pointers never cross the boundary.
The payload remains capped at 64 MiB, with explicit module/item/namespace and
string limits. Section order and count, parent connectivity, reference bounds,
identifier bytes, tags, trailing bytes, and the envelope invariants all
validate before HIR mutation.

The compiler driver exposes this checkpoint through private, explicit
commands. `--emit-cmhir` performs source loading, module-graph construction,
import resolution, HIR lowering, artifact capture, deterministic encoding,
and atomic publication. Repeated `--extern-cmhir` arguments decode dependencies
into fresh runtime identities before lowering the consumer. Missing or corrupt
dependencies, unsupported producer HIR, and input/output aliases reject while
preserving an existing output. This is useful process-independent HIR evidence,
but there is no object/archive production or link orchestration behind it.

Decode first validates the complete bounded wire graph without touching HIR.
It then opens a `CmHirContextMark`, creates the crate/module tree, reserves all
item and enum-variant definitions, restores owner-contiguous generic
parameters and defaults, materializes child-first structural types, binds ADT
and alias payloads, and restores the namespace snapshot against remapped
same-context DefIds. Alias and structural cycles reject. Restoration state is
built in a temporary artifact; only a committed HIR mark permits the final
artifact swap. Failure destroys the candidate and rewinds HIR. Loaded spans
use a caller-supplied metadata source with normalized zero offsets; the extern
alias is likewise supplied at load time rather than embedded.

The current fixtures start from parsed Rust and prove runtime-ID remapping,
producer-context destruction, `Wrapper<T = u32>`, generic enum `Choice<T>`,
`Alias = Wrapper<u32>`, fields, variants, child-module reexports, builtin
primitives, alias normalization, and exact rollback after CRC, truncation,
prevalidation conflicts, or post-mark semantic corruption. Encoding
canonicalizes modules by full
declaration path, items by owner path and declared name, and namespace
entries by owner path, exported name, wire kind, and canonical target; every
one-based handle is remapped after sorting. Two fresh producer processes
deliberately reverse HIR module/item creation and owned module/namespace
insertion yet write byte-identical nonempty files. A fresh prepopulated
consumer proves nonzero runtime-ID remapping and lookup after loading those
bytes. Separate driver processes produce, load, and republish a dependent
artifact, including authenticated external ADT lookup. Traits and associated
declarations, predicates, projections, impls, bodies, dependency-backed type
edges, free values/functions, macros, dependency archives, native-link
metadata, and a separately hashed object/C artifact remain broader bootstrap
requirements. Consequently M3-04 remains active despite deterministic
cross-process bytes and a real dependent declaration compile.

### Scratch type inference kernel

`CmTypeckContext` is a body/session-owned scratch type graph rather than
persistent HIR mutation. It imports supported HIR terms once, creates general,
integer, and float inference variables, and structurally unifies primitives,
references, pointers, tuples, arrays, slices, function pointers, ADTs, rigid
parameters, and projections. Rank-balanced union-find exposes a deterministic
minimum variable ID independently of its internal representative. Every
variable mutation is undo-trailed; public unification and HIR import open
private LIFO snapshots, so a class conflict, mismatch, occurs failure, or depth
overflow restores all bindings, terms, cached imports, and scratch storage.

Occurs checking uses an explicit worklist and visited type IDs, while
structural unification memoizes resolved type pairs. This matters because the
scratch representation is a shared DAG rather than mrustc's predominantly
owned type tree. Recursive import, unification, and freeze have a conservative
depth bound and return an explicit overflow status instead of consuming an
unbounded C stack. Scratch arrays use 16-byte arena alignment for conservative
32-bit C ABI compatibility.

Freezing is allowed only for a fully solved term under an active mark belonging
to the same HIR context. A private nested mark makes failure atomic. Imported
HIR identities are guarded by both the HIR arena lifetime and a monotonic
rewind generation, preventing stale-ID reuse after rollback or context
reinitialization. Region identities are preserved on imported/frozen terms,
but structural equality deliberately does not equate them: compatibility must
be emitted as a later region obligation, matching the original compiler's
erased-lifetime inference behavior without inventing a region solver here.

Exact-owner instantiation imports a HIR type or named type while substituting
only the lifetime, type, and const parameters owned by one authenticated item.
Trait/impl `Self` is a separate authenticated binding; unmatched `Self` rejects
and foreign parameters remain rigid. Const arguments are transactionally
unified with their declared parameter types. One per-call memo preserves shared
HIR DAG structure, and every failure restores scratch terms and bindings.

This kernel is not yet a general type-checking pass. There is no general body
constraint generation, coercion/defaulting, const/value inference variable,
method lookup, or diagnostic integration. A separate immutable impl index can
select one positive ordinary impl
transactionally. Its narrow generic
slice admits only owner-authenticated type parameters that are all constrained
by the impl self type or trait arguments, instantiates them with fresh inference
variables for each trial, and recreates only a unique winner. Required,
binder-free implemented-trait predicates can be instantiated under that same
candidate substitution and recursively solved through a dependency-inverted
canonical-table callback; every other predicate form remains blocking. An
immutable parameter environment captures
exact- and enclosing-owner predicates, outlives facts, trait `Self`,
supertraits, and positive impl headers from the same authenticated HIR
snapshot. Its supported implemented facts materialize under separately
authenticated substitutions and are tested before impl-index candidates;
blocked higher-ranked, projection, modifier, mixed-owner, foreign-owner, and
overflow cases remain explicit rather than being skipped.

An immutable goal table is bound to that environment, impl index, HIR snapshot,
and impl-universe mode. Implemented-trait keys contain structural resolved
self/trait arguments plus exact/enclosing substitutions and explicit
universe/de Bruijn binder identity. First-occurrence numbering alpha-renames
type and lifetime inference variables, while iterative tri-color traversal and
structural node interning make keys independent of scratch IDs and DAG sharing.
Deterministic non-proof results may be cached. `PROVEN` is not cached until a
canonical proof response can be replayed safely; it always reruns the
transactional solver. `OVERFLOW`, invalid API state, and typeck failures are
not cached. An ordinary evaluating-table re-entry yields ambiguity, never
success, and taints every active entry so an SCC-dependent non-proof cannot
poison a later independent proof.

The same table now owns domain-separated projection-equality goals. A proof
first selects and recursively proves one unique impl, authenticates that
impl's associated definition, instantiates its target under the replayed
substitution, and recursively normalizes a root projection target through the
same table. Projection and expected terms both participate in canonical keys.
Candidate trials and every non-proof roll back all scratch state and evidence;
overlap remains ambiguous even if only one candidate's target equals the
expected type. Unique target mismatch is `NO_SOLUTION`, while GATs, nested
structural projections, associated defaults, specialization, incomplete
foreign metadata, and malformed identities retain distinct fail-closed
outcomes.

A generation-bound semantic session composes one impl index, exact/enclosing
parameter environment, canonical table, and session-owned type-check graph.
All components authenticate the same HIR shape and become stale after an
observable append or rewind. The bounded executable pipeline now publishes the
complete reachable HIR closure before creating sessions, instantiates each
supported callee predicate from its exact call substitution, solves it in the
caller's environment, and admits MIR construction only after every reachable
call obligation is proved. This is a real semantic barrier for the existing
zero-or-one-`u32` instance slice, not yet general expression constraint
generation or post-typecheck rewriting.

Before any per-body semantic session or MIR is created, a whole-local-crate
item-conformance pass validates the supported positive trait-impl slice. It
follows authenticated associated-item DefId links, substitutes the trait's
concrete type arguments and `Self`, requires each targetless associated type
and required method exactly once, permits omission of default methods, and
checks every supplied method's receiver, parameter and return types, ABI,
safety, constness, asyncness, and variadic flag. Only parameters owned by the
exact trait declaration may appear in its signature; impl headers, associated
targets, and nongeneric impl methods reject rigid or foreign parameters. The
pass is read-only and runs only after reachable HIR publication, so failure
leaves existing output files and fresh output paths untouched.

Generic impls, GATs, generic methods, higher-ranked or outlives predicates,
associated-type bounds, projections, associated-type defaults, negative or
auto traits, cross-crate trait declarations, and unsupported associated-item
kinds return explicit pending or unsupported states rather than silently
passing. Specialization is already rejected explicitly during HIR lowering
because HIR does not represent that flag. Default method overrides are still
checked against their declarations; a declaration's body is not itself a
reason to reject the impl.

The remaining semantic pipeline is deliberately layered. The immutable
parameter environment feeds canonical binder-aware tabled goals; generic impl
candidates are instantiated and replayed transactionally; their predicates are
solved recursively; projections normalize only through proved selections; and
dependency metadata must authenticate a closed impl universe before absence can
mean no solution. From there the dependency order is:

1. enter one crate-wide transaction over every cfg-active local body, without
   reachability filtering or MIR construction;
2. generate and finalize general expression constraints;
3. resolve value/type/trait paths and normalize associated projections;
4. compute coercions and record immutable adjustments;
5. select methods, operators, and callable traits;
6. type patterns, binding modes, and coverage;
7. infer and lower closures, coroutines, and async state;
8. perform post-typecheck rewrites and validate codegen readiness;
9. solve region/outlives obligations and apply conservative borrow acceptance;
10. publish the complete typed-HIR barrier, then evaluate constants and lower
    the validated body set to broad MIR.

Const evaluation and conservative region/borrow checks consume typed results
rather than being guessed by lowering. The result lattice distinguishes proof,
negative proof, no solution, ambiguity, deferred inference, deferred metadata,
unsupported semantics, and overflow. Unknown or cyclic ordinary goals never
count as success; coinduction is restricted to explicitly modeled auto-trait
recursion after negative candidates have been considered.

### Bounded impl/projection and method checkpoint

The current checkpoint supports exact ordered-nominal selection for local,
positive impls of local, nongeneric traits. After lowering normalizes type
aliases, the generic shape is exactly:

```rust
impl<T, U, ...> Trait for LocalNominal<T, U, ...> {
    type Assoc = ...;
}
```

Impl generics are type-only and have no defaults. The impl-generic count must
equal both the nominal-generic count and query-argument count, and the self
arguments must be those parameters exactly once in declaration order.
Repeated, reordered, nested, fixed, omitted, and unused impl parameters are
outside this checkpoint. Monomorphic scalar and zero-argument local struct or
enum selection remains supported. Trait children may be bare, targetless
associated type declarations or ordinary non-generic Rust-ABI methods. Impl
children may be bare, target-bearing associated type definitions or matching
method definitions. Every required declaration must have exactly one
definition, except that a trait method or associated const with an explicit
default-body promise may be omitted or overridden even when declaration-only
HIR has no executable body. Impl and trait safety must match, and extra or duplicate
definitions are errors. Each impl association records the DefId of its trait
declaration. Both `Self::Assoc` and qualified `<Self as Trait>::Assoc` lower to
the same definition-backed projection shape.

Projection selection has two deliberately different APIs:

- `cm_hir_match_projection` takes a read-only HIR context. It is allocation-free
  and returns the matched target template, query self type, impl DefId, and
  impl-association DefId. Its explicit `local_crate` argument identifies the
  crate requesting selection.
- `cm_hir_select_projection` takes a mutable HIR context. It first matches and
  then instantiates the target template, returning the concrete target,
  identities, underlying HIR status, and allocated-type count. It uses the
  same explicit caller-crate boundary.

Instantiation reuses the query argument TypeId without allocation for a direct
target such as `type Assoc = T`. Structural targets such as `Pair<T, T>` append
changed types to HIR without modifying existing nodes. Ordinary substitution
failure rewinds both type-vector and arena additions. It exposes no target or
DefIds and reports the underlying non-OK HIR status when available.

The APIs report one of eight outcomes:

- `SELECTED`: one exact impl and association matched; matching returns its
  template and mutable selection returns the instantiated concrete target.
- `DEFERRED_ARGUMENTS`: arguments or an unsupported but potentially applicable
  candidate prevent an exact decision. Such a candidate is never skipped in
  favor of another candidate.
- `DEFERRED_SELF`: the self type is not concrete enough for exact selection.
- `DEFERRED_CRATE`: the trait or nominal self type is foreign to the caller, or
  a potentially matching foreign impl prevents a local decision. Unrelated
  foreign impls do not prevent selection.
- `NO_IMPL`: no exact or potentially applicable candidate exists.
- `AMBIGUOUS`: multiple exact candidates exist; declaration order is never a
  tie-breaker.
- `SUBSTITUTION_FAILURE`: a matched template could not be instantiated; no
  partial result is published.
- `INVALID_ASSOCIATION`: the selected impl does not provide a valid matching
  associated definition.

All non-selected outcomes expose neither a target nor DefIds. Matching performs
no unification, predicate or where-clause evaluation, obligation solving,
specialization, defaulting, alias normalization, or recursive projection
selection. Trait arguments, associated/GAT arguments, blanket
`impl<T> Trait for T`, lifetime/const impl generics, and cross-crate selection
also remain outside the checkpoint. Cross-crate cases defer rather than being
treated as absent or selecting an implementation. Alias normalization is the
lowering caller's responsibility before candidate lookup. Associated const
selection remains unsupported; the bounded inherent declarations retained by
graph lowering are structural HIR only.

Methods are represented without pretending that their bodies have semantic
HIR yet. A receiver remains parameter zero and is classified as value, shared
reference, mutable reference, custom typed, or absent. `mut self` is a mutable
local; `&mut self` is an immutable binding with a mutable-reference type.
Typed receivers require a plain `self` binding. Symbolic `Self` carries the
DefId of its containing trait or impl, and the model rejects cross-owner uses.
Impl methods carry the matching trait-method DefId. Lowering runs eight phases:
trait header, trait associated types, trait methods, impl header, other roots,
impl associated types, impl associated consts, then impl methods. This makes
parent generics and associated types available before value types and method
signatures lower.

An unlowered body is identified by both source and AST-local expression ID;
graph tests deliberately use colliding expression IDs from different files.
Body locals retain receiver binding mutability, types, and an explicit source
parameter index. A wildcard parameter remains a typed signature/ABI position
but creates no name-resolution local; compact body-local storage therefore
cannot turn `_` into a binding or lose the original parameter position.
Expanded-AST lowering preserves effective outer item/method attributes as
ordered structural metadata. Preservation alone does not implement an
attribute's semantics: later consumers must handle effectful attributes such
as `track_caller` or hard-error. Raw lowering rejects attributed methods.

Source loading has a bounded entry point that never buffers more than the
caller's byte ceiling and leaves the source set unchanged on I/O, allocation,
or size failure. Item-position include mechanics use that entry point with a
32-level, 256-file, 16 MiB aggregate budget. Included fragments are parsed into
the invoking unit's AST and carry a parallel source-ID map, so lexical splicing
does not collapse item, attribute, namespace, or diagnostic provenance.
Source-set storage is reacquired by ID after loading because appending an
included file may relocate the source array.

The source-fixture adapter remains available as an explicit no-binding test
mode. Normal graph options instead authenticate production include expansion.
A shallow pass retains inline modules and loads only external `#[macro_use]`
dependencies, including one simple relative `#[path = "..."]` override. It
constructs declaration-ordered textual histories and exact macro exports,
resolves each pending source call to one source-qualified declaration, and
uses the module macro namespace only when textual scope has no binding. The
exact `macro_rules! include` declaration must carry exactly one bare
`rustc_builtin_macro` attribute before bounded I/O begins. Successful splicing
destroys all derived graph storage and contextual plans, then repeats staging;
unsupported or ambiguous behavior publishes no effective view. This moves the
authentic Rust 1.90 core frontier through `core/src/lib.rs:421` and the rustdoc
primitive placeholder `impl ! {}` in `primitive_docs.rs`. The impl parser maps
that placeholder to an inherent impl whose self type is the never type, while
retaining `!Trait` as the negative trait-impl marker. Apostrophe
disambiguation requires an identifier-start character literal to
close immediately, preventing a lifetime in `define_bignum!` from swallowing
later delimiters. Block-local
const declarations are owned by block item statements but never enter root or
module effective-item lists; `const { ... }` remains a distinct
block-expression form. HIR body lowering rejects both until local definitions
and const evaluation can preserve their semantics. An outer attribute before
a block expression is owned by the final expression node with its exact ID and
span, including across postfix chains; dumps expose this ownership, while HIR
body lowering rejects attributed expressions before mutation. Inner
attributes remain a distinct style and reject outside crate/module starts.
Outer attributes before `let` are retained on the statement itself rather
than being parsed as a let-condition expression. Block-local statics use the
same item-statement ownership as local consts and remain absent from module
namespaces. These two forms clear the last parser errors in the canonical
x86_64 Rust 1.90 core graph. Bare cfg atoms use key-presence semantics,
including when the matching environment entries carry values, so the guarded
`atomic_int!` declaration is active alongside its value-selected invocations.
All 363 reachable sources now construct 451 modules without a graph error. HIR
rejects attributed statements and local item semantics until their effects are
modeled.

The resolver records `extern crate` aliases under their published name. The
special `extern crate self as core` form targets the crate-root module and is
available from nested modules through the extern-prelude lookup, while its
source `self` name is never published as an ordinary binding. HIR `hir-v18`
retains that declaration as structural import metadata with one type-namespace
alias targeting the real crate-root DefId; it creates no synthetic item.

Raw enum variants retain their ordered outer attributes and exact source spans.
The module graph applies its authoritative cfg set through the shared
cfg/cfg_attr attribute processor and publishes an effective variant view. Each
entry keeps the enum's source-qualified declaration plus its original AST
index, so inactive variants disappear without renumbering identity. Import
resolution traverses this view for enum self, explicit variants, aliases,
globs, and checked paths. Every active variant enters the type namespace; unit
and tuple constructors also enter the value namespace. This clears all 38
unresolved core imports. Same-target coalescing and explicit builtin primitive
identity then clear the remaining canonical diagnostics: the target-configured
core import graph has zero unresolved, ambiguous, or cyclic imports. HIR still
rejects variant import bindings until its canonical import target can store a
variant identity distinct from the parent enum definition. After structural
self-crate lowering, `hir-v18` stores effective outer attributes on the real
child module record, separately from its inner attributes and without a
synthetic module item. A publishing module now authenticates each effective
declaration through its per-item source map rather than requiring the
declaration source to equal the module root source. This keeps included
declarations in their shared syntax unit while preserving their actual file
identity; included inline-module children inherit the same source-qualified
declaration. Whole-core HIR therefore passes `primitive_docs.rs`. An item-owned
outlives-predicate slice stores either a type or lifetime subject
and one region bound; validation authenticates early-bound regions against the
item or parent generic scope, while higher-ranked binders remain unsupported.
Trait and outlives predicate order is deterministic within their separate
arrays in `hir-v18`. Free functions use the same trait-predicate lowering as
trait declarations and methods, which clears the mixed callable, `Copy`, and
`'static` constraint in `contracts.rs`. Graph HIR now admits source-written and
authenticated macro-generated root consts among root value declarations. Their
real item definition owns the declared type and an unlowered body retaining the
exact graph-owned initializer ID. Immutable, explicitly typed,
initializer-bearing source statics use the same representation; mutable and
generated statics remain rejected. Simple resolved const names in array
lengths retain an unevaluated reference to the real const DefId. Source
declarations retain their own spans; generated item, type, attribute, and body
spans use the source invocation anchor. Same-root generated functions now use
the ordinary function reservation and lowering path: their real DefId owns
effective attributes, the substituted signature and parameter locals, and an
unlowered body naming the exact expression appended to the graph-owned module
AST. Include-origin generated bodies remain rejected because syntax ownership
and their diagnostic invocation span currently have different sources.
Inherent impl headers store the resolved self type
without a trait identity and do not participate in trait completeness or
candidate selection. Their non-default, immutable, explicitly typed,
initializer-bearing const children retain the real associated DefId, parent
impl, effective metadata, and graph-owned initializer as an unlowered body;
immutable, explicitly typed, targetless trait const declarations retain a
child DefId, trait-owned `Self` type, and no body. Defaults and impl definitions
remain closed until value items gain an explicit trait-declaration link. This
clears `f128::RADIX`, `f128::consts::PI`, `POWER_OF_FIVE_128`, and
`Integer::{ZERO, ONE}`. This clears the generated signed and unsigned integer
square-root functions. A source-written `unsafe extern "C"` block is now a
graph-only container whose cfg-active bodyless function children become
ordinary root HIR functions with their real DefIds, declared visibility and
types, effective structural attributes, exact child spans, and inherited `C`
or `unadjusted` ABI. A child without an explicit safety qualifier is unsafe;
an explicit `safe fn` remains safe. The exact lint-only block attribute
`allow(improper_ctypes)` is admitted and intentionally ignored because the
container owns no HIR item and the lint has no checking or code-generation
effect. No block item, fake parent, body, or implementation is synthesized.
This clears `num/libm.rs`, the attributed `link_name` declarations in
`fxsr.rs`, and the cfg-active unadjusted intrinsic blocks; other block
attributes, ABIs, and unsupported foreign shapes still reject transactionally.
Generated inline modules now reuse the graph-created child and mapped HIR
module definition, retain the invocation span and effective outer/inner
attributes, and lower their generated children without allocating a duplicate
module item or DefId. This clears the `mod private` from
`impl_zeroable_primitive!`. Root anonymous consts retain distinct definitions,
types, attributes, spans, and initializer bodies while introducing no value
namespace name, matching the graph's existing rule and clearing
`define_valid_range_type!`. Source-written, targetless, non-generic foreign
types inside authenticated extern-C blocks become root `EXTERN_TYPE` items;
they retain their DefId, visibility, effective attributes, and span, and type
paths to them use `CM_HIR_TYPE_FOREIGN_KIND`. This clears `type VTable;` in
`ptr/metadata.rs:165`. Local generated-item provenance now authenticates both
named `macro_rules!` and declarative `macro` definitions, clearing the
recursive `marker_impls!` expansions in `marker.rs`. HIR `hir-v21` adds a
predicate-owned lifetime binder for bound-level `F: for<'a> Trait<...>`
constraints. Binder names and spans are provenance; semantic identity is the
declaration-order index beneath that predicate binder. Predicate-prefix
`for<'a> F: Trait<'a> + Other<'a> + 'a` constraints instead retain one
item-owned scope referenced by every atomic trait/outlives constraint and
shared with the subject. Validation authenticates scope storage, subject,
exact atom counts, references, spans, and all late regions together. Nested
prefix/bound binders remain closed until regions carry binder depth as well as
variable index. Generic inherent methods retain their receiver, method
generics, where predicates, signature, parent impl, and body without a
synthetic trait-method link. This clears `VaListImpl::with_copy` in
`ffi/va_list.rs:246` and the `GenericShunt` predicate in
`iter/adapters/mod.rs:155`. Graph-authenticated inherent methods may carry
explicit visibility and `const`, retaining their parent, effective attributes,
Rust ABI, signature flag, and graph-owned unlowered body. Trait and trait-impl
const methods remain closed. This clears `pub const fn is_nan` in
`num/f128.rs:293`. HIR `hir-v21` adds a trait-declaration link to value items.
Macro-generated trait-impl associated const definitions bind that link to the
exact targetless declaration and retain their impl parent, `Self`-owned type,
effective provenance, and graph-owned initializer body. Model validation and
impl completeness reject absent, mismatched, or duplicate links. This clears
`Integer::{ZERO, ONE}` from `int!(u16, u32, u64)` in
`num/dec2flt/float.rs:48`. Trait associated const defaults retain a
declaration-owned type, an explicit default-body promise, and, when source is
available, a graph-owned unlowered body. Impl completeness uses the promise:
it requires one definition for a required const but allows zero or one
override for a default. This clears `SIG_BITS` and the following `RawFloat`
defaults.
The next whole-core barrier is the generic trait-impl method `Hash::hash<H>`
in `num/nonzero.rs:291`; positive trait-impl methods now reuse method-owned
generic and predicate scopes, and model validation checks linked declaration
generic arity. The next barrier is the `extern "rust-call"` trait method
`AsyncFn::async_call`; associated methods now structurally retain either the
ordinary Rust ABI or explicit `rust-call`, and linked trait/impl methods must
agree. This clears `ops/async_function.rs:14`. The next barrier is the generic
associated type `AsyncFnMut::CallRefFuture<'a>`. Associated type declarations
and positive-trait impl definitions now reuse the item's generic and predicate
arenas. `Self` projections may resolve through supertraits while retaining the
trait that actually defines the selected associated type, and impl links must
match declaration generic arity. This preserves `Future<Output =
Self::Output>` and `Self: 'a`, clearing `ops/async_function.rs:28`. The next
barrier is the attributed async trait method `AsyncDrop::drop`. Async trait
declarations now preserve effective attributes, the async signature bit,
custom receiver, and absence of a body. A linked impl method must carry the
same async bit. This clears `future/async_drop.rs:40`. Enum variants now carry
their own canonical DefIds. Lowering pre-reserves those identities by parent
enum and source index so structural imports can refer to them before the enum
is bound, then atomically binds each identity to the final enum item/index.
Import validation admits type bindings for every form, value bindings only for
unit and tuple constructors, and rechecks already-retained imports at enum
binding. This clears the variant bindings in the prelude glob at
`core/src/lib.rs:214` (`item=2`, span `7763..7789`). Expanded-away named macro
definitions now receive definition-only canonical HIR DefIds when a structural
import first references them. Their authenticated graph identity retains the
owner module, declared name, and macro form; aliases reuse the same DefId and
the model confines it to the macro namespace. This clears the `Copy` binding
in that same glob. Const generic declarations now retain the parsed type in a
dedicated AST field and lower it as the required declared type of an ordered,
owner-scoped HIR const parameter. A bounded const-default slice retains exact
text plus structural expression identity and lowers plain prior-parameter,
root-const, or inherent-associated-const paths to typed HIR const arguments;
other expressions fail closed. `where T: ?Sized` updates the generic
parameter's relaxed-sized property without fabricating a trait predicate.
This clears both `AtomicOrdering` parameters of `atomic_cxchg` at
`core/src/intrinsics/mod.rs:96` and `{ Assume::NOTHING }` in `TransmuteFrom` at
`core/src/mem/transmutability.rs:90`. Inline lifetime bounds on generic type
or lifetime parameters lower into the existing item outlives-predicate model;
the subject keeps its type-versus-lifetime kind, source order is stable, each
bound keeps its exact span, and named regions resolve to the owning early-bound
parameter. This clears `T: ?Sized + 'static` on `type_id` at
`core/src/intrinsics/mod.rs:2757`. Trait methods may now own lifetime generics;
receiver and argument regions retain the same early-bound identity and default
bodies stay graph-owned, clearing `Error::provide<'a>` at
`core/src/error.rs:204`. Trait-owned lifetime parameters are also admitted,
and a lifetime supertrait lowers as a structural `Self: 'a` outlives predicate
instead of a nominal supertrait edge. This clears
`unsafe trait Erased<'a>: 'a` at `core/src/error.rs:971`. Trailing type
defaults on ADTs clear `pub enum ControlFlow<B, C = ()>` at
`core/src/ops/control_flow.rs:87`. Trait methods may now own ordered const
parameters such as `SpecArrayClone::clone<const N: usize>`, and array types
retain `[Self; N]` with a typed `CM_HIR_CONST_PARAMETER` length. Function
generic defaults, non-const length parameters, and const parameters whose
declared type is not `usize` remain fail-closed.

Boundless, nongeneric targetless associated-type declarations now enter a
transaction-marked projection registry before trait defaults lower. The
registry authenticates reserved trait/associated DefIds and exact generic
argument kinds without publishing an item; the normal phase later publishes
the declaration once, preserving canonical arena order. This clears
`FromResidual<R = <Self as Try>::Residual>` in either source order while
defaulted supertraits, bounded/GAT declarations, duplicates, wrong arity/kind,
and graph rollback remain checked. An authenticated inherited implicit prelude
now clears the `Sized` supertrait on `FullOps` at
`core/src/num/bignum.rs:23`. The GCC- and TinyCC-built whole-core probes agree
that ordered positional supertrait arguments and associated equalities now
retain their resolved definitions, RHS types, exact spans, and ownership.
Literal `'static` supertraits lower as structural `Self: 'static` predicates;
undeclared, inferred, and wrong-kind lifetime bounds still fail closed. These
steps clear `ops::Shr<u32, Output = Self>` at
`core/src/num/dec2flt/float.rs:20`. Trait aliases now preserve ordered trait
and lifetime bounds, generic arguments, associated equalities, modifiers, and
exact spans; alias chains and alias use in predicates/supertraits retain their
resolved identities, while mixed cycles and impl headers reject. Auto traits
retain an authenticated flag, and negative impls retain polarity, generics,
predicates, types, identities, and spans; only the safe itemless non-const
auto-trait form is admitted. Structural generic defaults on trait references
recursively substitute `Self` plus earlier lifetime and type arguments while
preserving shared HIR DAGs. Explicit `'static`, early-bound, and authenticated
predicate-binder lifetime arguments are retained. Const trait arguments remain
unsupported, so const-default substitution is an internal invariant rather
than an end-to-end source claim. Associated-type trait bounds now retain
positional lifetime/type arguments plus Self-free trailing defaults in their
named trait type, while equality bindings remain a separate ordered array.
Wrong order/kind/arity, const arguments, nested constraints, GAT equalities,
and defaults needing the bounded associated-type `Self` subject reject.
Associated lifetime bounds now become ordinary type-outlives predicates whose
subject is one authenticated `<Self as Parent>::Associated` projection;
`'static` and parent-owned early-bound regions retain identity, while inferred,
free, wrong-owner, late-bound-without-a-binder, and GAT cases reject. Both
probes now stop at `core/src/marker.rs:895` (item 44, span `33718..33727`),
where an associated trait bound's generic default needs that same projection
as its `Self` substitution. Alias expansion into canonical obligations remains
part of the trait solver rather than declaration lowering.

Match-arm `if let` guards retain a separate pattern, initializer expression,
and full guard span while ordinary expression guards keep their existing
representation. Dumps distinguish the forms, and HIR body preflight validates
then rejects structured let guards until match semantics exist. Method-call
turbofish syntax owns ordered generic arguments and the exact `::<...>` span;
dumps preserve them and HIR body preflight rejects generic method calls until
method substitution exists. Lifetime where-predicate subjects and bounds
retain a distinct AST kind and interned
payload; HIR lowering rejects them explicitly until lifetime constraints can
be preserved semantically. Opaque `impl Trait` types own ordered, individually
spanned trait bounds and reuse the callable-trait representation for inputs
and `Output`; HIR lowering validates the structure and rejects it until an
existential type model exists.
Postfix `?` has a distinct AST node owning its complete operand and span; HIR
body preflight validates the node and rejects it until residual control flow
is represented. Block statement lookahead traverses balanced outer attributes
before deciding whether `const IDENT` is a local item; normal item parsing then
owns the attributes, while module namespaces and effective views still exclude
the local declaration.
Tuple-index projection has a distinct expression node rather than fabricating
a named field: it owns its base, parsed `uint32_t` ordinal, exact selector span,
and full projection span. HIR body preflight validates that structure and
rejects it until tuple type/layout semantics can lower the ordinal faithfully.
The lexer retains significant-dot context through trivia so chains such as
`value.0.1` and `value.0.field` do not collapse into float tokens.
Match-arm bodies use a statement-expression restriction: once an
unparenthesized block-like expression is complete, the Pratt parser stops
before postfix and infix operators. The arm separator check can then apply
Rust's optional-comma rule without consuming a following tuple or slice
pattern as a call or index.
Qualified expression paths have a distinct node carrying the self type,
optional trait path, associated expression path, exact `<...>` qualifier span,
and full expression span. This covers both `<T as Trait>::item` and
`<T>::item`. HIR body preflight validates and rejects them before ordinary
local or free-function lookup, including when the node is a call callee.
Block statement item lookahead recognizes local functions through balanced
outer attributes and the supported `const`, `async`, and `unsafe` modifiers.
Normal item parsing retains the complete function, but root ownership, module
namespaces, and effective item views continue to exclude the local declaration.

The macro AST records a form discriminator rather than inferring definition
semantics from a name. Declarative `macro` items preserve their name,
visibility, attributes, optional parameter token tree, body, delimiter, and
span. Their token trees are deliberately opaque: the real Rust 1.90 macro
module can be represented, while expansion and planning remain fail-closed
until a supported declarative macro implementation exists.

Item-macro planning consumes named definitions from the ordinary semantic item
tree but retains them in a separate cfg-active declaration array. Each record
owns its effective outer attributes and records the immediate inline-module
container, macro form, span, and generated-item provenance. This keeps
compiler metadata available for exact export/builtin authentication without
making macro definitions appear as lowerable Rust items.

During module construction, the graph filters this flat declaration array by
the module's exact inline-container identity and registers matching entries in
the macro namespace. External source roots use the empty container identity;
inline modules use their source-qualified `mod` item. This is local namespace
registration only: export and macro-use edges still require authenticated
scope construction.

The module graph owns an effective item tree. Each root and recursively
recorded trait/impl child receives a nonzero opaque effective-item ID local to
one graph revision and module. Effective items expose their ID, child kind, and
direct-child count. Direct-child lookup takes the parent ID, and attribute
lookup accepts either a root or recursive-child ID; the older indexed root
attribute accessor delegates through the root ID. IDs are allocated
monotonically while the effective tree is recorded, but callers must treat them
as opaque. Module contents remain roots of their own module and are not copied
into a parent's recursive children.

Recursive children come from item-macro plan children after cfg and cfg_attr
processing. A disabled associated type or method is absent rather than marked
inactive, and HIR completeness checks only the resulting active trait/impl
sets. Thus a cfg-disabled required trait method does not require an impl
definition, while omitting an active required method still fails the graph
transaction. Effective views are graph-owned and revision-checked, clear their
outputs on error, and cannot be consumed after a failed graph build.

Effective attributes retain order, metadata bodies, source attribute identity,
owner, span, and expansion depth. HIR copies the metadata, source attribute
identity, span, and expansion depth from item-owning graph roots and recursive
children; the containing HIR item supplies the owner. A `use` declaration is
instead retained as module-owned structural metadata containing its raw tree,
declared visibility, effective outer attributes, and resolver-produced
namespace bindings. It creates neither a definition nor a synthetic HIR item.
Module declarations similarly store effective outer attributes on their mapped
child `CmHirModule`, separately from inner attributes owned by module contents;
they do not create a duplicate item or DefId. Preservation is not semantic
interpretation: the import record does not itself perform downstream lookup,
enforce visibility, or establish anonymous-import trait scope.
Source-backed effective nodes retain their source item. Generated roots and
children have no source item identity and instead retain their immediate
producing macro invocation, macro definition, and expansion depth. Their span
uses the outermost source-backed invocation as a coarse diagnostic anchor.

Graph validation recursively checks effective identity, child kind/count,
source or generated provenance, and attributes. Reservation then creates
trait/impl children from their effective IDs. Lowering retains the ordered
eight phases above. Generated trait/impl roots and generated associated methods
therefore use the same path as source-backed nodes. Generated method bodies
retain source-qualified AST expression IDs, and impl methods retain their
matching trait-method DefId. These are identity and declaration contracts, not
expression semantics.

This checkpoint does not lower expressions, resolve method calls, perform
dispatch, compare impl signatures semantically, or execute default bodies.
Method generics and where clauses, non-Rust ABIs, const/async methods, pattern
parameters beyond simple bindings and wildcard discards, associated-type
bounds/predicates, trait associated consts, and inherent associated consts
outside the bounded initializer-bearing definition shape remain hard errors.
Trait supertraits are the narrow exception: ordinary required and `~const` bounds
are retained in source order as resolved `CmHirNamedType` records,
each with its source span and exact modifier. Model insertion accepts reserved
local item definitions for forward resolution, rejects self-edges and
already-known non-trait targets, prevents an inbound target from later binding
as a non-trait or with an incompatible generic arity/kind signature, and
rejects an edge that completes a cycle. Reachability and final colored graph
validation use explicit work stacks, so shared DAGs are visited linearly and
deep valid chains do not consume the C call stack. Final lowering requires
every edge to resolve to a bound local or authenticated producer trait and
rejects duplicate direct edges. This is not supertrait-obligation solving or
conditional-const selection. Symbolic-
`Self` projection queries return `DEFERRED_SELF` rather than selecting a
concrete impl.

The pinned Rust 1.90 `core`/`alloc` census contains 407 explicit
associated-type impls, 385 of them method-bearing. Fifty-eight fit a broader
nominal/no-where/no-trait-argument header funnel. Tightening that to the exact
ordered type-only shape accepted by the current parser and lowerer, and
excluding two unsupported `impl const` forms, leaves 14 impls. All 14 contain
methods (21 methods total). Twelve impl parents have attributes: ten ordinary
(`stable` or `unstable`) and two cfg-only. Thirteen methods across eight impls
carry ordinary `inline` attributes. The union is 12 of 14 impls requiring an
ordinary attribute on the parent or a method, with the remaining two cfg-only;
the exact set has no cfg_attr and no generated impls or methods. Two methods
use typed receivers (`Pending::poll(self: Pin<&mut Self>, ...)` and
`Ready::poll(mut self: Pin<&mut Self>, ...)`); the other 19 have simple
non-self parameter shapes. All 21 are non-generic, have no method where clause,
and are safe, non-const, non-async, non-extern, and non-variadic. Four
signatures contain a supported single `Self::Assoc` projection.

The 14 impls/21 methods group as follows:

| Trait | Impls | Methods |
|---|---:|---:|
| `Future` | 2 | 2 |
| `Iterator` | 5 | 10 |
| `Try` | 2 | 4 |
| `IntoIterator` | 2 | 2 |
| `Deref` | 1 | 1 |
| `AsVecIntoIter` | 2 | 2 |

Every impl requires its trait definition to be active and lowered.
`AsVecIntoIter` was the first compact compatible definition: its
children are attribute-free, it has no supertrait or bounded associated type,
and unsafe trait/impl safety is represented. An extracted three-file fixture
from `library/alloc/src/vec/in_place_collect.rs:428` and
`library/alloc/src/vec/into_iter.rs:531` now exercises the unsafe headers,
sibling-module reexports, ordered generic impl, `Self::Item` return projection,
mutable receiver, source-qualified body, and active/inactive
`cfg(not(no_global_oom_handling))` topology together. It simplifies the
surrounding `IntoIter` definition but retains the upstream `#[stable]`
attribute on its public reexport. Both that declaration and the `pub(crate)`
reexport are retained structurally with their resolved bindings; this does not
claim that later HIR consumers enforce their accessibility. It has one
associated type and one attribute-free method; the sibling impl at
`library/alloc/src/collections/binary_heap/mod.rs:1680`, guarded by
`cfg(not(test))`, is a second fixture.

The exact Rust 1.90 `Future` declaration and both `Ready<T>` and `Pending<T>`
impl signatures form a second admitted family. Their source-backed graph
attributes were already retained. `Ready::poll(mut self: Pin<&mut Self>, ...)`
required mutable custom-receiver lowering; custom receiver types now follow a
spine of references and single-type-argument nominal wrappers to the enclosing
trait/impl `Self`. The check is repeated after alias normalization, so an
unrelated or alias-erased receiver cannot survive. This does not prove the
later `Receiver` trait obligation. `Pending::poll(..., _: &mut Context<'_>)`
retains its wildcard as a nameless discard signature parameter and deliberately
omits it from body locals, while `Future::poll`'s `cx` and `Ready::poll`'s `_cx`
remain ordinary named bindings. The fixture also retains the exact
`Pending<T>` field type `PhantomData<fn() -> T>`, upstream struct/impl
attributes, signature linkage, and source provenance. Method bodies remain
unlowered; none of this claims expression semantics or dispatch.

The selected `Try` slice is now admitted as a third family. Its trait
declaration is the Rust 1.90 declaration shape. The fixture also retains the
upstream trait-level `unstable`, `rustc_on_unimplemented`, `doc(alias)`,
`lang`, `const_trait`, and `rustc_const_unstable` attributes and the associated
item `unstable`/`lang` attributes; the compact rendering below shows the exact
type and method signatures:

```rust
pub trait Try: ~const FromResidual {
    type Output;
    type Residual;
    fn from_output(output: Self::Output) -> Self;
    fn branch(self) -> ControlFlow<Self::Residual, Self::Output>;
}
```

The admitted impl signatures are likewise exact extracts:

```rust
impl<B, C> ops::Try for ControlFlow<B, C> {
    type Output = C;
    type Residual = ControlFlow<B, convert::Infallible>;
    fn from_output(output: Self::Output) -> Self {}
    fn branch(self) -> ControlFlow<Self::Residual, Self::Output> {}
}

impl<T> Try for NeverShortCircuit<T> {
    type Output = T;
    type Residual = NeverShortCircuitResidual;
    fn branch(self) -> ControlFlow<NeverShortCircuitResidual, T> {}
    fn from_output(x: T) -> Self {}
}
```

The `ControlFlow` impl retains its upstream `unstable` attribute, and all four
impl methods retain `inline`; their bodies are intentionally empty.

Only the surrounding `FromResidual` declaration is deliberately simplified:
Rust 1.90 has generic `FromResidual<R = <Self as Try>::Residual>`, while this
fixture uses a nongeneric empty `FromResidual`. Consequently this checkpoint
does not claim its generic default/projections, supertrait obligations,
conditional-const selection, or the full upstream `try_trait.rs`. The
whole-core path now admits that exact generic default through the authenticated
targetless-associated registry described above. This does not implement its
runtime obligations or conditional-const selection. The
canonical HIR dump includes every supertrait's owning item, source index,
modifier, resolved named type (and arguments), and span.

The exact Rust 1.90 `IntoIterator` and `Deref` declarations form a fourth
admitted family:

```rust
pub trait IntoIterator {
    type Item;
    type IntoIter: Iterator<Item = Self::Item>;
    fn into_iter(self) -> Self::IntoIter;
}

pub trait Deref: PointeeSized {
    type Target: ?Sized;
    fn deref(&self) -> &Self::Target;
}
```

An associated declaration owns source-ordered bound records. Each required or
relaxed record retains its resolved named trait, exact span, and ordered nested
equalities; each equality retains the resolved target associated definition,
RHS HIR type, and its own span. `Self` in an equality RHS is rooted in the
enclosing trait, not the associated declaration. HIR insertion deep-copies the
nested records and revalidates inbound forward references when reserved
definitions bind. Lowering accepts nongeneric local or authenticated producer
trait bounds,
nongeneric type equalities, and the exact relaxed `?Sized` form. Defaults,
GATs, positional arguments, duplicate identities, other relaxed bounds, and
wrong-kind targets hard-error. The canonical format is `hir-v33`.

The next source-backed fixture retains the exact Rust 1.90 attributes and
signatures of 68 `Iterator` methods. Trait and trait-method type parameters are
definition-owned HIR records. Inline `T: Bound + Bound` entries precede
explicit `where` predicates, and every atomic predicate retains its subject,
resolved named trait with fully materialized arguments, associated equalities,
and source span. Equalities resolve through supertraits, so callable notation
such as `FnMut(A) -> B` stores a one-element tuple argument on `FnMut` and the
inherited `FnOnce::Output = B` identity. The AST marks only tuples synthesized
by callable notation; ordinary one-element source tuple/group syntax remains a
hard error. Shorthand projections such as `U::IntoIter` resolve from active
item predicates, and omitted trailing trait arguments materialize supported
defaults such as `PartialOrd<Rhs = Self>`. Default method bodies are minimized
in the fixture, but the explicit required-versus-default promise remains
structural independently of executable body availability. Item reservations
carry an expected HIR item kind;
pre-bind `Self` types are admitted only for definitions explicitly reserved as
traits or impls, and binding a different item kind hard-errors.

These declarations do not imply obligation solving, method applicability,
normalization, unsizing, or dispatch. The remaining nine `Iterator` methods
require const or lifetime method generics, nested associated constraints, or
`impl Trait`. Source-backed module-graph HIR retains attributed associated
types and methods; raw lowering and standalone expanded associated-type
lowering remain deliberately narrower for attributes.

Selection currently scans the entire HIR item vector for an impl, then scans
it again for the selected associated definition. A query is therefore
`O(number of HIR items)` with two linear scans and no index. Lowering validation
also scans existing HIR items to check completeness and duplicate candidates.
These checkpoint-scale scans need keyed indexes before corpus-scale use.

Module-graph lowering is transactional: a late failure rewinds HIR vectors,
the arena, the interner, result IDs, and the module map. Direct AST lowering
and expanded/cfg-view lowering are not transactional, so late validation
failures may leave partial HIR in the caller-owned context. Expanded lowering
and module-graph lowering both consume cfg-active child sets. Graph lowering
additionally consumes the revision-bound recursive effective tree and remains
transactional across validation, reservation, and all ordered lowering phases.
Neither path lowers expressions, resolves method calls, performs dispatch,
compares impl signatures semantically, or generates code.

## Differential gates

Each layer gains a canonical dump format with generated IDs and paths
normalized. Tests progress in this order:

1. token kind/text/span streams;
2. parsed AST;
3. expanded and resolved AST/HIR;
4. inferred HIR and selected impls;
5. unoptimized MIR;
6. bidirectional metadata reading with the oracle;
7. normalized generated C and ABI facts;
8. executable behavior under the same C compiler;
9. the same generated C compiled and linked by TinyCC;
10. crate-group and full-bootstrap gates.

The corpus grows from focused probes to existing local tests, Rust library
crates, compiler crates, and finally the complete pinned bootstrap graph.
