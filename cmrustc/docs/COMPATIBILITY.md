# Compatibility policy

`cmrustc` accepts a bootstrap dialect of Rust. The dialect is a superset of
the valid syntax needed by the pinned upstream source and may accept programs
that upstream `rustc` rejects. Acceptance is not permission to miscompile
programs that are valid in the bootstrap corpus.

## Compatibility classes

| Class | Policy | Example |
|---|---|---|
| Runtime-critical | Must match Rust semantics | layout, drop, calls, traits, atomics |
| Compile-time erasure | May be accepted without checking | lifetimes, borrow conflicts, lints |
| Conservative approximation | May do extra safe work | copies, leaks, retained temporaries |
| Source patch | Allowed when explicit and reproducible | build scripts, unsupported target code |
| Unsupported ambiguity | Hard compiler error | unresolved impl or unknown ABI |

The bounded ordered-nominal projection checkpoint makes conservative decisions.
An unsupported candidate that could apply reports `DEFERRED_ARGUMENTS` and is
not skipped in favor of a supported candidate. Multiple exact candidates
report `AMBIGUOUS`; declaration order never chooses a winner. `NO_IMPL` is
reserved for the absence of both exact and potentially applicable candidates,
and `SUBSTITUTION_FAILURE` never exposes a partial target or implementation
identity. None of these outcomes permits choosing or fabricating an
implementation.

Projection APIs take the requesting crate explicitly. A foreign trait or
nominal self type, or a potentially matching foreign impl, reports
`DEFERRED_CRATE`; an unrelated foreign impl does not poison a local exact
selection. Cross-crate coherence and metadata are not inferred from the
trait's defining crate.

The supported generic shape is only
`impl<T, U, ...> Trait for LocalNominal<T, U, ...>` with type-only, default-free
impl parameters used exactly once and in order as the nominal arguments.
Repeated, reordered, nested, fixed, omitted, or unused parameters are excluded,
as are predicates/where clauses, specialization, defaults, lifetime or const
generics, GATs, trait arguments, blanket impls, cross-crate selection, and
recursive or alias-normalizing matching. Monomorphic scalar and zero-argument
local nominal impls remain in scope. This checkpoint is not broad Rust 1.90
trait compatibility. Of 407 explicit associated-type impls in pinned Rust 1.90
`core` and `alloc`, 385 contain methods. Fifty-eight fit a broader nominal
header funnel; the exact parser-compatible ordered type-only boundary leaves
14 non-`impl const` forms, and all 14 contain methods (21 total). These are
shape counts, not proof of expression semantics, method resolution/dispatch,
code generation, or selection.

Ordinary non-generic trait and impl methods now have declaration-preserving
HIR. Receivers remain parameter zero, impl methods link to the corresponding
trait-method identity, symbolic `Self` is owner-qualified, and required/default
method completeness is checked. Bodies are preserved only as source-qualified
unlowered expression identities; this does not provide expression semantics,
method lookup, or dispatch. Expanded lowering and item-owning graph
roots/children preserve ordered effective structural attributes, including
metadata bodies and source provenance, but effectful attributes must later be
implemented or rejected. Graph `use` declarations are module-owned structural
records retaining their raw tree, visibility, effective attributes, and
resolved namespace bindings. They are not definitions or synthetic HIR items,
and retention alone does not implement downstream lookup, visibility
enforcement, or anonymous-import trait scope. Attributed graph module
declarations still hard-error because modules do not retain their outer
declaration attributes.
Raw attributed methods still hard-error. Graph-owned nonzero effective-item
IDs identify roots and recursive trait/impl children within one revision and
module; direct-child and attribute lookup are revision-checked. Nested cfg and
cfg_attr are applied before graph publication, so disabled associated items are
omitted and completeness is checked only over the active child set. Generated
trait/impl roots and children follow the same graph/HIR path, retain immediate
macro provenance and source-qualified body identities, and do not acquire a
false source-item identity. Method generics/where clauses, non-Rust
ABIs, const/async methods, and complex parameter patterns also remain outside
the supported dialect.

In the exact 14-impl/21-method Rust 1.90 set, 12 impl parents have attributes
(ten ordinary and two cfg-only), and 13 methods across eight impls have
ordinary `inline` attributes. Twelve of 14 impls therefore require ordinary
attributes somewhere on the parent or a method; the other two are cfg-only.
There are no cfg_attr or generated impl/method occurrences. Two methods have
typed receivers and the other 19 have simple non-self parameter shapes. All 21
are non-generic, have no method where clauses, and are safe, non-const,
non-async, non-extern, and non-variadic; four signatures contain a supported
single `Self::Assoc` projection. The trait groups are `Future` (2 impls/2
methods), `Iterator` (5/10), `Try` (2/4), `IntoIterator` (2/2), `Deref` (1/1),
and `AsVecIntoIter` (2/2).

All 14 require their trait definition to be active and lowered.
`AsVecIntoIter` was the first compact compatible definition. An extracted
three-file fixture from `library/alloc/src/vec/in_place_collect.rs:428` and
`library/alloc/src/vec/into_iter.rs:531` now admits its unsafe trait/impl,
sibling-module reexports, ordered generic impl, `Self::Item` projection,
mutable receiver, body identity, and `cfg(not(no_global_oom_handling))`
topology together. It simplifies the full `IntoIter` declaration while
retaining the relevant import structure: upstream `#[stable]`, the public
reexport, and the `pub(crate)` reexport are retained with resolved bindings.
This is a structural record, not proof that later HIR consumers enforce import
accessibility. It has one associated type and one attribute-free method.
The sibling
binary-heap impl at `library/alloc/src/collections/binary_heap/mod.rs:1680`
under `cfg(not(test))` is the second fixture.

The exact Rust 1.90 `Future` trait and `Ready<T>` and `Pending<T>` impl
signatures are now also admitted. `mut self: Pin<&mut Self>` is represented as
a custom receiver with a mutable body local, and the receiver type must remain
rooted in the correct trait/impl `Self` through references or single-argument
nominal wrappers both before and after alias normalization. This structural
check does not replace future `Receiver` trait solving. Bare `_` parameters
retain their typed signature position as an explicit discard and create no
body local; underscore-prefixed `_cx` remains a normal named binding. The
fixture retains the exact `Pending<T>` `PhantomData<fn() -> T>` field,
upstream attributes and signatures, while minimizing surrounding declarations
and leaving method bodies unlowered.

The selected `Try` slice now admits the exact Rust 1.90 declaration
`trait Try: ~const FromResidual` with `Output`, `Residual`, `from_output`, and
`branch`, plus the exact `ControlFlow<B, C>` and `NeverShortCircuit<T>` `Try`
impl signatures. The fixture deliberately reduces the surrounding generic
`FromResidual<R = <Self as Try>::Residual>` trait to a nongeneric empty trait;
it does not claim generic defaults, obligations, conditional-const selection,
or the full upstream `try_trait.rs`. The whole-core path now admits that exact
generic default by authenticating `Try::Residual` before trait defaults, but
does not claim its obligations or conditional-const behavior. Supertrait
records preserve local resolved
named targets, source order, spans, and required versus `~const` modifiers.
Reserved local definitions permit forward edges. HIR insertion prevents those
targets from later becoming non-traits or acquiring incompatible generic
signatures and rejects cycle-completing edges; explicit work stacks keep graph
validation linear and independent of C recursion depth. Final lowering rejects
unresolved/non-trait targets and duplicate direct edges.

The exact Rust 1.90 `IntoIterator` and `Deref` declaration shapes are also
admitted. `IntoIter: Iterator<Item = Self::Item>` retains an ordered required
bound, resolved `Iterator` and `Iterator::Item` definitions, the equality RHS
projection, and exact spans. `Target: ?Sized` retains a relaxed bound on the
resolved local `Sized` support trait; `Deref: PointeeSized` uses the existing
supertrait representation. Attributes on both traits, associated types, and
methods retain graph provenance. The accepted associated-bound slice is local
and nongeneric: defaults, GATs, positional bound arguments, arbitrary relaxed
bounds, and obligation/normalization semantics hard-error. These records and
supertraits appear in canonical `hir-v15` dumps.

The resolver authenticates at most one cfg-active private root glob carrying
the exact effective `#[prelude_import]` attribute. Its fully resolved public
bindings are consulted as a lowest-priority fallback for unqualified lookup in
every module without publishing synthetic child-module bindings. Ordinary
local/imported bindings shadow the fallback, while private exports, qualified
paths, ambiguity, duplicate attributes/imports, nested/public declarations,
and non-glob forms fail closed. This admits the inherited `Sized` supertrait on
`FullOps` in `core/src/num/bignum.rs`; no name-specific `Sized` rule exists.
Ordered positional arguments and associated equalities now clear
`ops::Shr<u32, Output = Self>` at `core/src/num/dec2flt/float.rs:20`, and a
literal `'static` supertrait becomes structural `Self: 'static`. Trait aliases
now retain ordered trait/lifetime bounds, generic arguments, associated
equalities, modifiers, and exact spans; aliases are valid in predicates and
ordinary supertraits but reject in impl headers, and mixed cycles reject. This
clears `Thin` at `core/src/ptr/metadata.rs:84`. Auto-trait identity and
negative-impl polarity are structural HIR, and structural trait defaults plus
authenticated explicit lifetime arguments clear the intervening declarations.
Associated-type trait bounds retain positional lifetime/type arguments and
Self-free trailing defaults independently of associated equalities. The next
whole-core declaration frontier is the lifetime associated-type bound in
`type Unsigned: Copy + 'static` at `core/src/intrinsics/fallback.rs:13` (item
1, span `502..509`).

The source-backed Rust 1.90 `Iterator` fixture now retains the exact attributes
and signatures of 68 methods. The admitted declaration slice includes
trait/method type parameters, ordered inline and explicit predicates,
positional trait arguments, associated equalities, generic supertraits,
shorthand parameter projections, callable `Fn*` notation, and supported
trailing trait type defaults. Callable input tuples carry explicit AST
provenance so `FnMut(T)` lowers to a `FnMut` trait reference whose positional
argument is a one-element tuple, without admitting ambiguous
ordinary `(T)`/`(T,)` source syntax. Predicates and equalities retain resolved
DefIds, arguments, source order, and exact spans in deterministic `hir-v15`
output. Default bodies are deliberately minimized while preserving
required-versus-default body state. This structural storage does not claim
obligation solving or method applicability. Nine methods remain excluded:
three with const generics, three with lifetime generics, and three with nested
associated constraints or `impl Trait`. Raw and standalone-expanded entry
points remain narrower than source-backed graph HIR for active attributes.

Named struct literals now parse with explicit fields, shorthand fields, empty
forms, and a final update base. Control-expression brace disambiguation follows
the Rust grammar boundary, while parentheses and other delimited children
re-enable struct literals. Unsafe blocks retain their marker and are not
mistaken for `unsafe { ... }` struct construction. The exact Rust 1.90
`core::future::pending` source is
retained as a byte-identical parser fixture. HIR `hir-v15` can store a complete
construction of one bound local nongeneric named struct, with authenticated
field ordinals, exact child types and owners, source-ordered spans, deep-copy
ownership, and atomic rejection. Source-body HIR lowering also admits complete
local nongeneric named structs through direct, qualified, or imported-alias
paths, recursively types heterogeneous and nested fields, supports empty named
structs, enforces visibility, and transactionally rejects stale resolver
snapshots or malformed coverage without publishing partial expressions. Direct
named-field reads preserve exact base/name/full spans and lower from aggregate
parameters, fresh constructions, or nested chains to a `FIELD` node carrying
the authenticated base, aggregate DefId, declaration ordinal, and exact
declared result type. Numeric tuple fields reject explicitly. Update semantics,
generics and cross-crate construction remain unsupported and are not inferred
from this HIR checkpoint. A separate checked
layout query admits only i32/u32 leaves and recursively nested same-crate local
nongeneric named structs, returning exact declaration-order offsets under an
explicit 32- or 64-bit address model. It atomically rejects empty, generic,
tuple, union, represented, recursive, cross-crate, and unsupported-leaf shapes;
it is layout authority rather than permission for an exported aggregate ABI.

MIR `mir-v6` lowers the admitted direct-body aggregate slice into bounded,
flattened local places. Each place carries an exact type/span and up to 16
authenticated aggregate-DefId/declaration-ordinal projections; a local with no
projections is also valid. Initializers execute in source order but aggregate
rvalues store a complete declaration-ordered field array. Scalar i32/u32 field
reads use copy-place operands, aggregate-valued reads use move-place operands,
and nested aggregates retain their actual nominal temporary types. Stored MIR
owns deep copies of all aggregate and projection slices. Internal monomorphic
calls may take one or two exact u32 or authenticated aggregate parameters and
must return exact u32. Each argument is evaluated and materialized before the
call terminator; aggregate operands move by value. Aggregate returns, generic
or cross-crate aggregate calls, projected writes, and exported aggregate ABI
still reject rather than infer behavior.

The portable C99 emitter accepts those aggregates only as reachable locals and
temporaries behind scalar-u32 exported signatures. Before output it discovers
the recursive nominal closure, runs the checked target layout query, and emits
dependency-first ordinal-named structs with `sizeof`, `offsetof`, and alignment
proofs. Aggregate construction uses declaration-ordered designated compound
literals, and flattened field places use ordinal member chains. Reordered and
nested construction plus a two-step read execute under GCC and TinyCC 0.9.27.
Checked aggregate parameters are emitted by value only on internal static
functions; a mixed aggregate/u32 call returning u32 executes in both compilers
while the exported declaration remains scalar-only. Generic, cross-crate,
represented, recursive, or empty layouts, aggregate returns or exports, and
projected writes remain unsupported.

The legacy executable dialect slice accepts only an exact no-core program: the
crate attributes are exactly `feature(no_core)`, `no_core`, and `no_main`, and
the sole active item is a public, safe, nongeneric, zero-parameter,
`#[no_mangle] extern "C" fn main() -> i32` whose body tail is one suffixed,
in-range `i32` literal.

The current reachable-call slice accepts one generic identity body
`fn identity<T>(x: T) -> T { x }` instantiated exactly as u32, plus ordinary
monomorphic callees with one or two exact u32 or checked same-crate aggregate
parameters and an exact u32 result. Scalar call arguments may be parameter
locals or recursive u32 addition/subtraction trees of parameter locals and
exact u32 literals; aggregate arguments may be authenticated constructions or
aggregate locals. Admitted calls may be nested inside those arithmetic trees or
inside another call's arguments. Independently computed subexpressions execute
left-to-right; each nested call ends one MIR block, stores its result in an
exact temporary, and continues in the next block before the outer call or root
addition executes. All public no-mangle functions are
roots. Private unsupported functions outside their closure are omitted;
unsupported roots, unresolved callees, missing or extra substitutions, other
types, recursion, operators other than wrapping u32 `+`, `-`, and the exact
conditional equality described below, and other call shapes hard-error without
publishing an artifact. The same path also accepts
top-level immutable identifier lets with an explicit exact `u32` type, an
initializer from the authenticated expression vocabulary, no shadowing, and a
required tail expression. Initializers execute in source order, can read only
parameters and earlier lets, and can contain direct calls. User locals precede
generated temporaries in MIR, and stored MIR is replayed exactly against the
HIR block before emission. Mutation, destructuring, inferred types,
self/forward reads, nested statement blocks, and control flow other than the
exact value-producing diamond below hard-error. These boundaries are narrower
than Rust, but every accepted
program has observed
generated-C execution; unsupported constructs are not replaced with values or
no-ops.

The current arithmetic slice additionally accepts an exact public
`#[no_mangle] extern "C" fn add(left: u32, right: u32) -> u32` whose body is
`left + right`, exact u32 subtraction, and recursively nested u32 arithmetic
trees whose leaves are parameter locals or decimal literals explicitly
suffixed `u32` or inferred from an already-exact u32 context.
Executable MIR uses ordered,
single-assignment u32 temporaries for non-root binary values and emits those
assignments through portable C. `+` and `-` are defined with Rust wrapping-u32
behavior. Signed subtraction, mixed scalar types, context-free literal
defaulting, non-decimal or otherwise unsupported bare literals, malformed
temporary graphs, general statements, and other expression forms hard-error
on a reachable root. A private unsupported body outside root reachability
remains omitted rather than guessed. Canonical dumps are `hir-v31` and
`mir-v9`.

The all-local body manifest can now prove `MARKED -> REGIONS` for this bounded
expression slice. The proof is read-only and covers bounded MARKED evidence,
owner signatures/value declarations, body expected/local types, every
reachable expression type, direct-call substitutions, and recursively nested
type/const/generic arguments. Static, erased, and correctly scoped early-bound
regions are accepted; unresolved regions/types/consts and malformed or
aliased graphs fail without changing phase, generation, or capability. This
does not implement original mrustc's lifetime inference, region equality/
outlives inference, promotion eligibility, or borrow checking. Item
predicates, declaration bounds/fields, type-position unevaluated array
expressions, and explicit enum discriminants remain outside this rooted proof.
Manifest body owners and enclosing trait/impl items with predicates or
outlives constraints reject instead of silently escaping that proof;
the latter two still lack manifest atom identities.

Canonical `hir-v30` adds stable, context-owned source-closure identities,
closure-owned lexical parameters, exact visible-local prefixes, nominal types
keyed by closure identity, and source-closure/parameter expression nodes.
Signatures are reserved before those dependent nodes and bodies bind once;
rewind, finalization, deterministic dumps, typed barrier fingerprints, and
observer currentness authenticate the arena. This is representation only:
capture absence/class and Copy evidence are uncomputed, while invocation,
lifetime inference, expansion, MIR, and C emission reject these nodes.

Canonical `hir-v31` preserves the compiler-authenticated const capability of
traits and the exact constness of trait impl headers. Const inherent impls and
const impls of non-const traits reject; legacy semantic metadata v1.1 rejects
these facts instead of silently erasing them.

Every current function, const, and static body still has an `ITEM_SOURCE`
origin whose definition, enclosing definition, item backlink, and legacy owner
agree exactly. Exact canonical callable instances additionally
encode separate dispatch and executable-body definitions. Semantic results,
admission, MIR, reachability, and C emission preserve both, and `mir-v9` dumps
render `dispatch/body`. Qualified receiver-free calls can now execute a
nongeneric trait default through a concrete local impl. The trait method remains
both selected callable and executable body, while the selected impl, self
owner/type, and complete canonical argument domains preserve concrete dispatch;
two impls sharing one body therefore remain distinct MIR/C instances. Bodyful
overrides win, bodyless linked overrides block fallback, and ambiguous linked
overrides reject. Dot-method defaults, calls within default bodies, meaningful
`Self`, generic traits, source-level blanket inherited defaults, multiple
bodies per owner, type-position bodies, and generated closure or promotion
bodies remain unsupported.

The first exact control-flow slice accepts `if left == right { then_u32 }
else { else_u32 }`. Equality is restricted to exact u32 operands and produces
an internal bool; the two blocks must assign one u32 destination and converge
at one return join. Generated C stores bool only as exact `uint8_t` 0 or 1 and
does not expose it through an ABI. The executable canary combines equality,
wrapping addition, and wrapping subtraction across ordinary, overflow, and
underflow boundaries. `!=`, i32 equality, missing `else`, and noncanonical
diamonds reject atomically.

The complete strict GCC, TinyCC 0.9.27, and GCC ASan/UBSan/leak matrices pass.
GCC- and TCC-built compilers emit an identical 493-byte artifact with SHA-256
`e0dce3f5cd322c89f64787a28a307095de2f27d8c8791cc5f01233e81d9275d7`.

The first target-width scalar slice accepts exact `usize` parameters, returns,
explicit immutable lets, decimal `usize` literals, wrapping `+` and `-`, and
same-module monomorphic one- or two-argument calls whose complete scalar
signature is `usize`. It also accepts one exact value conditional:
`if left < right { then_usize } else { else_usize }`. HIR retains the literal
value through `UINT64_MAX` without choosing a target width. MIR must be
qualified as 32- or 64-bit before publishing any reachable `usize`, rejects
32-bit overflow, and cannot later change width.

Generated C represents `usize` as `uintptr_t`, never host `size_t`. Constants
use explicit `UINT32_C` or `UINT64_C` payloads cast to `uintptr_t`, and every
translation unit using `usize` proves `UINTPTR_MAX` and `sizeof(uintptr_t)`
against the selected target. A MIR/target mismatch or compiling target-32 C on
a 64-bit host therefore fails closed. Mixed u32/usize signatures or
arithmetic, context-free literal defaulting, usize generic substitution,
aggregate usize source calls, methods, casts, shifts, division, and bool ABI
remain unsupported and reject without publishing an artifact.

The complete strict GCC, TinyCC 0.9.27, and GCC ASan/UBSan/leak matrices pass.
GCC- and TCC-built compilers emit identical target-specific C; the x86-64
`const_min` executable canary is 1,277 bytes with SHA-256
`0260d79f545b2b2646f9f458166e04ead626e9cae78ca8da2e60f14e3e09b90f`.

Across `i32`, `u32`, and `usize`, an unsuffixed bare decimal literal may take
its type only from an already-exact return, explicit immutable let, aggregate
field, branch/operator, or direct-call argument context. Matching explicit
suffixes remain accepted and `usize` still validates against the selected
target width. This is contextual propagation, not type-variable unification or
Rust's fallback defaulting: annotation-free lets, conflicting suffixes, hex,
underscored, floating, overflowing, and unsupported expected-type cases reject
transactionally.

A separate scratch type engine now supports general/integer/float variables,
transactional structural unification, occurs checks, HIR import, and solved
freeze for the supported primitive and structural type forms. Region identity
is preserved but ignored by this equality layer pending explicit region
obligations. Deep/shared DAGs are bounded or visited, stale HIR sessions reject,
and all failed operations roll back. Exact-owner instantiation substitutes
lifetime/type/const arguments and separately authenticated `Self`, keeps
foreign parameters rigid, validates const argument types, preserves shared HIR
DAGs, and is atomic. A separate open-universe impl index transactionally
selects only unique positive ordinary impls; type-only impl generics are
replayed with fresh inference variables and required binder-free implemented
predicates are solved recursively. All unsupported candidates block and
absence defers metadata. Immutable parameter
environments retain exact/enclosing predicates, outlives facts, trait `Self`,
supertraits, and positive impl headers from an authenticated HIR snapshot.
Supported assumptions materialize and replay transactionally; higher-ranked,
projection, modifier, recursive, mixed-owner, foreign-owner, and overflow cases
remain explicit blockers or pending goals. A snapshot-bound canonical goal
table structurally keys implemented obligations, alpha-renames inference
variables, distinguishes binder universe/de Bruijn identity, and bounds
iterative DAG canonicalization. It caches only deterministic non-proof results;
proofs are replayed through the transactional solver and ordinary table cycles
never prove themselves. Projection-equality goals use domain-separated keys
containing both sides, prove one unique impl and its predicates, authenticate
the impl-associated definition, instantiate its target, and recursively
normalize root projection targets. Non-proofs roll back and never expose
associated-definition evidence; overlap is ambiguous even when only one target
matches, and incomplete foreign metadata remains deferred. Implemented-trait
dispatch is wired only into the bounded reachable-call obligation barrier;
projection equalities still reject there pending session dispatch. None of this
is general body constraint generation, Rust fallback, coercion, const
inference, method resolution, or whole-body typing.

Intrinsic source cfg now comes only from a registered canonical target
descriptor. The admitted facts cover target architecture, OS, environment,
ABI, vendor, family, pointer width, endianness, baseline x86 features,
thread-local support, and the registered atomic classes for i386, i686, and
x86-64 Linux targets. Contradictory caller-built descriptors reject. Edition
selection affects parsing but is not a Rust cfg fact; `edition = "..."` and
`rust_20xx` are not accepted as implicit predicates. Crate features,
command-line cfg values, panic mode, sanitizers, and codegen options are not
inferred.

Production item-position include splicing is resolver-authenticated. The graph
first selects one exact cfg-active macro declaration through textual scope or
an unambiguous macro-namespace binding, then requires that exact source
`macro_rules! include` declaration to carry exactly one bare
`rustc_builtin_macro` attribute. Only an unqualified, unattributed,
parenthesized ordinary relative string literal ending in a semicolon is opened.
The path remains bounded to depth 32, 256 files, and 16 MiB, and every splice
discards derived graph state and contextual plans before staging repeats.
Malformed, duplicate, shadowed, disabled, qualified, nonliteral, generated,
and repeated-source cases hard-error without opening an unauthenticated path.
The separate fixture adapter retains its stricter no-binding mode. Authenticated
Rust 1.90 core now completes its empty-cfg module graph with 293 source files,
378 modules, and zero errors. This proves source discovery, cfg filtering,
macro planning, and effective graph publication only; it does not imply HIR,
type, metadata, code-generation, or executable-library support.

Rust 1.90 alloc now completes the same empty-cfg graph gate with all 62 source
files, 67 modules, and zero errors when the successful core graph is supplied
as an authenticated dependency-macro artifact. Generic-parameter outer
attributes retain exact order, text, and spans, including `#[may_dangle]`;
HIR still rejects them until attribute semantics are modeled. The lexer
distinguishes a bounded UTF-8 character literal such as `'Σ'` from a Unicode
lifetime without claiming full XID validation. Nested empty repetition
backtracking and separator-at-end progress follow the real `to_string_str!`
shape, and type-position macro invocations are retained structurally. HIR does
not yet expand those type macros.

`$crate` is retained distinctly until transcription. An exact
resolver-certified macro binding may supply one validated ASCII defining-crate
identifier, and recursive generated invocations preserve it; local or null
bindings expand to `crate`, while guessed paths reject. Dependency macro
lookup has a live-graph artifact boundary: it revision-checks a successful
dependency graph, requires a structured public path, and returns the exact
source-qualified declaration and borrowed macro AST without reusing source IDs
in the consumer namespace. The consumer graph accepts an artifact array only
during its synchronous build. An unqualified source invocation requires one
exact explicit external use leaf. Grouped aliases work; local collisions,
dependency-rooted globs, competing imports/artifacts, private paths,
stale/null/self artifacts, and duplicate extern or hygienic identifiers reject.
Unrelated globs do not prevent an exact import, and source-written hygienic
crate paths remain unauthorized.

Generated lookup keeps hygienic paths separate: `rust_core::...` is accepted
only through the authenticated defining-crate route, while the consumer extern
name `core::...` is not interchangeable with it. The module graph installs the
generated-only callback, allowing an imported outer macro to reach a different
`$crate::module::helper!` on demand without authorizing source calls or
enumerating guessed public macros. Private and ambiguous generated paths fail
closed with distinct classifications.

Planner AST ownership is wide enough to distinguish local source IDs from
graph-local external owners without truncation. Effective-item provenance has
a separate dependency-definition reference with the exact consumer
graph/generation, a per-definition publication certificate, an opaque
consumer-local dependency identity, exact dependency revision, and
dependency-scoped source declaration. Local macro provenance cannot reuse that
source ID. Revision-checked validation accepts only the final published
consumer snapshot; it rejects field splices, cross-graph or prior-generation
references, failed rebuilds, and definitions seen only in abandoned staging
rounds. The dependency graph and temporary artifact need not remain live after
the consumer copied its generated syntax. A separate published-import view
authenticates the exact consumer module, source-qualified use declaration,
local alias, invocation AST owner, and producing dependency definition. HIR
may ignore only that leaf's unresolved macro-namespace diagnostic; a grouped
sibling or unrelated unresolved leaf still rejects the entire transaction.
After those checks, supported body-free declarations and authenticated root
consts whose expanded AST is consumer-owned lower normally, including
invocations spliced from authenticated include files and snapshots whose
producer graph has been destroyed. A dependency macro that expands to no
effective syntax remains conservatively unresolved.

A first in-memory HIR library artifact copies one successfully lowered
producer's public module/type namespace, including same-crate public trait,
type, module, and builtin primitive reexports. Primitives retain an explicit
scalar identity and no invented DefId. The artifact retains loaded producer
DefIds without retaining the producer graph, ASTs, resolver, or HIR module map.
Fully qualified consumer paths such as `dep::api::Type` lower as foreign ADTs,
type aliases, extern types, primitives, or producer-owned traits in the same
HIR context; producer aliases
normalize to their loaded target. Artifact extern names are unique, local
definitions retain precedence, and private paths or invalid artifact arrays
reject before partial HIR survives. The producer HIR context must remain live.
An exact unresolved, non-glob type-namespace leaf can also be authenticated
against the artifact and stored in structural HIR as a binding to a producer
type, trait, or module DefId. Imported traits work in impl headers and bounds;
imported module aliases may qualify later type or trait lookup. Cross-crate
supertraits, generic trait arguments, associated bounds, impl associations,
and qualified projections retain producer DefIds and use the producer's bound
HIR signature. Aliases and grouped leaves retain source order. Local
collisions, competing unresolved leaves or artifacts, dependency-rooted globs,
private siblings, and unrelated resolver errors reject transactionally.
A private graph-independent snapshot builder now copies names and accepts
only live same-crate module/type bindings; successful restoration moves the
snapshot into an artifact, while every invalid crate/root, target, conflict,
or extern alias leaves the prior artifact and candidate allocations unchanged.
The `cmhir-meta-v1` semantic layer now uses fixed-order `CRAT`, `MODS`, `GPAR`,
`TYPE`, `ITEM`, and `NSPC` sections in the bounded little-endian, versioned,
CRC32 envelope. It round-trips a root-connected module tree, public extern
types, structs, unions, enums and variants, free aliases, owner-grouped
lifetime/type generics and type defaults, supported structural types, existing
module/type reexports, and builtin primitive bindings into fresh runtime
identities. The wire
uses one-based file-local handles and caller-supplied source/extern identity;
it contains no runtime DefIds, intern IDs, pointers, or source IDs. Decode
validates before mutation and uses a HIR mark plus temporary artifact so CRC,
truncation, and valid-CRC semantic conflicts preserve exact prior context and
artifact state.

Encoding canonicalizes modules by full declaration path, items by owner
path and declared name, and namespace entries by owner path, exported name,
wire kind, and canonical target, then remaps every one-based handle. Two fresh
producer processes deliberately reverse HIR module/item creation and owned
module/namespace insertion yet write byte-identical nonempty files. A fresh
prepopulated consumer proves nonzero runtime-ID remapping and lookup. Parsed
Rust producers now travel through the real graph/import/HIR path, and the
private `--emit-cmhir` plus repeated `--extern-cmhir` driver switches prove
atomic producer, fresh-process dependent-load, and dependent-republication
paths. Traits and associated declarations, predicates, projections, impls,
bodies, const generics, unevaluated constants, dependency-backed type edges,
values, external globs, transitive external reexports, object/archive inputs,
linking, and executable alloc remain unsupported.
This checkpoint therefore keeps M3-04 active.

Core also
passes `core/src/lib.rs:421` and parses the rustdoc primitive
placeholder `impl ! {}` in `primitive_docs.rs` as an inherent impl for the
never type without weakening negative trait impls. Lifetime where predicates
and bounds have distinct AST kinds and interned lifetime payloads, so the
`'static` constraint in `contracts.rs` is retained without fabricating a type
path. HIR `hir-v18` now preserves it as an item-owned outlives predicate with
a type subject and static region bound; lifetime subjects and early-bound
regions use the same structural representation. Higher-ranked binders remain
an explicit unsupported boundary. Block-local const declarations
now have statement ownership without leaking into root/module namespaces or
effective-item views, while `const { ... }` remains a distinct expression;
HIR body lowering rejects both transactionally. Outer block-expression
attributes retain exact IDs and spans on the complete expression chain and
appear in AST dumps; HIR body lowering rejects them transactionally until
attribute semantics exist. Inner attributes remain distinct and reject
outside crate/module starts. Match-arm `if let` guards retain their pattern,
initializer, and full span without changing ordinary expression guards;
HIR body lowering validates and rejects them transactionally until match
semantics exist. Method-call turbofish syntax retains ordered generic
arguments and the exact `::<...>` span and is rejected transactionally during
HIR body lowering until method substitution exists. Opaque `impl Trait` types
retain ordered, spanned trait bounds, including callable inputs and `Output`;
HIR lowering rejects them explicitly until existential types are represented.
Postfix `?` is structurally retained as an operand-owning expression and is
rejected transactionally during HIR body lowering until residual control flow
exists. Outer-attributed block-local const items are recognized through
balanced lookahead, retain normal item attributes, and remain absent from
module namespaces and effective views. Tuple-index projection is retained as
a base-owning expression with a numeric ordinal and exact selector span; HIR
body lowering rejects it transactionally until tuple layout semantics exist.
Numeric selectors chain with numeric or named fields, including across trivia,
without changing ordinary decimal float tokenization.
Block-like match arms apply statement-expression termination and may omit their
comma before tuple or slice patterns without greedily consuming the next
pattern as a call or index postfix. Qualified expression paths retain their
self type, optional trait, associated path, and qualifier span; HIR body
lowering validates and rejects them rather than aliasing an ordinary local or
free function. Block-local functions, including attributes and supported
modifiers, are retained as statement-owned items without entering module
views; block-local statics now follow the same ownership rule. Outer
attributes on a `let` belong to the statement, with exact IDs and spans,
instead of entering the let-condition parser; HIR body lowering rejects those
attributes transactionally. Together these forms remove every parser error
from the canonical x86_64 Rust 1.90 core graph. Bare cfg predicates observe a
matching key even when its entries carry values, activating the guarded
`atomic_int!` definition as well as its value-selected calls. The resulting
363-source, 451-module target-configured graph completes without an error; this
does not imply successful HIR lowering or executable core support.

Self-crate aliases are retained as crate-root module bindings. The exact
`extern crate self as core` form publishes `core` through the extern prelude in
nested modules without publishing the reserved name `self`. This clears 49
real-core import errors. Same-target coalescing and builtin primitive identity
subsequently reduce target-configured core import resolution to zero
unresolved, ambiguous, or cyclic imports. HIR `hir-v18` retains the self-crate
declaration as a structural type-namespace alias to the real crate-root DefId,
without creating a fake item. Module declarations retain effective outer
attributes on their real child module, separately from module inner attributes
and without a duplicate item or DefId. Graph syntax access now authenticates a
source-qualified declaration against the publishing module's per-item source
map, so `include!`-spliced declarations can share its AST storage without being
mistaken for declarations from the module root file. Included inline-module
children retain that same declaration source. HIR lowering consequently passes
all `primitive_docs.rs` declarations with exact item, attribute, module, and
body provenance. Item-owned type/lifetime outlives predicates and free-function
trait predicates then clear the exact `contracts.rs` constraint without
synthesizing paths or weakening higher-ranked rejection. Source-written and
authenticated macro-generated root const declarations now retain their real
item definition, effective attributes, type, and graph-owned initializer
identity as an unlowered HIR body. Immutable, explicitly typed,
initializer-bearing source statics use the same representation, while mutable
and generated statics remain rejected. A simple resolved const name in an
array length is retained as an unevaluated reference to its real DefId rather
than guessed or evaluated. Same-root generated functions retain their real
definition, effective metadata, substituted signature and parameters, and
exact graph-owned unlowered body expression. Include-origin generated bodies
remain rejected until their distinct syntax and diagnostic sources can both be
represented. Inherent impls retain their self type without inventing trait
identity, and non-default, immutable, explicitly typed, initializer-bearing
associated const definitions retain their parent, metadata, type, and
graph-owned unlowered body. Immutable, explicitly typed, targetless trait const
declarations retain their real child identity, trait-owned `Self` type, and no
body; defaults and impl definitions remain rejected. This clears `f128::RADIX`,
`f128::consts::PI`, `POWER_OF_FIVE_128`, and `Integer::{ZERO, ONE}`. The
generated `signed_fn!`/`unsigned_fn!` integer square-root declarations now
lower. Source-written unsafe `extern "C"` and `extern "unadjusted"` blocks now
publish their cfg-active bodyless function children as real root HIR functions
with the inherited ABI, effective structural attributes, and Rust's
default-unsafe foreign-declaration semantics; explicit `safe fn` declarations
remain safe. The exact lint-only
block attribute `allow(improper_ctypes)` is accepted and ignored because the
container itself does not invent a HIR item or DefId and the lint is
non-observable here. This clears the four declarations in `num/libm.rs`, the
attributed `link_name` declarations in `fxsr.rs`, and the cfg-active
unadjusted intrinsic blocks. Other ABIs, foreign statics, generated blocks,
other block attributes, and generic or body-bearing children remain
unsupported. Generated inline
modules reuse their authenticated graph child and mapped HIR
module definition, retaining their invocation span, effective outer/inner
attributes, and generated children without a duplicate module item. This
clears `impl_zeroable_primitive!`'s `mod private`. Root `const _` items now
retain unique DefIds and bodies without publishing a value name, so repeated
anonymous consts from `define_valid_range_type!` no longer collide.
Source-written targetless foreign types in authenticated extern-C blocks now
retain an `EXTERN_TYPE` item with effective attributes and resolve as
`CM_HIR_TYPE_FOREIGN_KIND`, clearing `type VTable;` in `ptr/metadata.rs:165`.
Generated-item provenance accepts named declarative `macro` definitions as
well as `macro_rules!`, clearing `marker_impls!` in `marker.rs`. Bound-level
higher-ranked trait bounds now retain predicate-owned binder names, order,
span, late-bound regions, and callable equalities. Generic inherent methods
retain their own generics and predicates without a fabricated trait-method
identity. Predicate-prefix binders retain a shared item-owned subject scope
referenced by every expanded trait/outlives atom, rather than copying a binder
onto one bound. This clears `VaListImpl::with_copy` in `ffi/va_list.rs:246` and
the `GenericShunt` predicate in `iter/adapters/mod.rs:155`.
Graph-authenticated inherent methods now retain explicit visibility and
`const`, including their effective attributes, Rust ABI, signature flag,
parent, and graph-owned unlowered body. Trait and trait-impl const methods
remain unsupported. This clears `pub const fn is_nan` in `num/f128.rs:293`.
Canonical HIR `hir-v21` gives value items a trait-declaration link.
Macro-generated trait-impl associated const definitions retain that exact
link, their impl parent, `Self`-owned type, effective provenance, and
graph-owned initializer body; missing, mismatched, and duplicate definitions
remain errors. This clears `Integer::{ZERO, ONE}` from
`int!(u16, u32, u64)` in `num/dec2flt/float.rs:48`. This slice also retains
optional trait associated const defaults as graph-owned
unlowered bodies. A targetless const requires exactly one impl definition;
a default permits zero or one override. This clears `SIG_BITS` and the
following `RawFloat` defaults. The next whole-core rejection is the generic
trait-impl method `Hash::hash<H>` in `num/nonzero.rs:291`; positive trait impl
methods now retain their method-owned generics and where predicates, with
generic arity checked against the linked trait declaration. The next
whole-core rejection is `AsyncFn::async_call`, an `extern "rust-call"` trait
method. Associated methods now retain Rust or explicit `rust-call` ABI, and
linked trait/impl definitions must agree. This clears
`ops/async_function.rs:14`. Generic associated type declarations in traits and
definitions in positive trait impls retain item-owned parameters and where
predicates. Declaration links require equal generic arity. Inherited
`Self::Assoc` projections retain the supertrait that defines the associated
type, so `AsyncFnMut::CallRefFuture<'a>` keeps its lifetime,
`Future<Output = Self::Output>` equality, and `Self: 'a` predicate. This clears
`ops/async_function.rs:28`. Attributed async trait declarations retain their
effective attributes, async signature flag, custom receiver, and bodyless
state; linked impl methods must agree on asyncness. This clears
`AsyncDrop::drop` at `future/async_drop.rs:40`. Enum variants now retain
canonical DefIds distinct from their parent enum. Explicit, aliased, and glob
bindings target those identities with form-aware type/value namespace checks.
This clears the variant bindings in the prelude glob at `core/src/lib.rs:214`
(`item=2`, span `7763..7789`). Imported named macros now retain canonical
definition-only DefIds authenticated from the graph declaration. The identity
records its owner module, declared name, and `macro_rules!` or declarative form;
aliases share it, and non-macro namespaces reject it. This clears the `Copy`
binding in that same glob. Typed const generic declarations now retain a
structural AST type and lower as ordered, owner-scoped HIR const parameters
with that exact declared type. Const defaults retain exact text and a
structural expression; plain prior const-parameter, root-const, and inherent
associated-const paths lower as typed const arguments with canonical DefIds,
while other expressions reject. The HIR setter requires structural declared-
type agreement with both the destination parameter and any referenced const
parameter, so a differently typed prior parameter cannot be relabeled by a
default. A generic `where T: ?Sized` is retained as the
parameter's relaxed-sized property rather than as a predicate. This clears
both `AtomicOrdering` parameters of `atomic_cxchg` at `intrinsics/mod.rs:96`
and `{ Assume::NOTHING }` on `TransmuteFrom` at
`mem/transmutability.rs:90`. Inline type/lifetime-parameter outlives bounds now
retain their ordered subject and region identities plus exact bound spans in
HIR. This
clears `T: ?Sized + 'static` on `type_id` at `intrinsics/mod.rs:2757`.
Lifetime-generic trait methods retain method-owned early-bound receiver and
argument regions plus their default bodies, clearing `Error::provide<'a>` at
`error.rs:204`. Trait-owned lifetime generics and lifetime supertraits retain
the same identity as a structural `Self: 'a` outlives predicate, clearing
`unsafe trait Erased<'a>: 'a` at `error.rs:971`. Literal `'static` supertraits
also lower structurally as `Self: 'static`; undeclared free, inferred, and
wrong-kind lifetime bounds remain fail-closed. Trailing type defaults on
structs, unions, and enums now retain
their HIR generic arguments. ADT applications fill omitted trailing primitive
defaults and substitute an already-supplied earlier type argument when the
default names that parameter; other structural defaults, const-generic ADT
applications, and wrong argument arity/kinds reject. This clears
`pub enum ControlFlow<B, C = ()>` at `ops/control_flow.rs:87`. Both GCC- and
TinyCC-built whole-core probes keep zero graph/import errors over 363 sources
and 451 modules. Trait methods now retain ordered const parameters such as
`SpecArrayClone::clone<const N: usize>`, and `[Self; N]` retains `N` as a typed
`CM_HIR_CONST_PARAMETER` array length. Non-const or non-`usize` length
parameters and function generic defaults remain fail-closed. Boundless,
nongeneric targetless associated types now register before trait defaults
without publishing HIR items, clearing
`FromResidual<R = <Self as Try>::Residual>` in either source order while
preserving defaulted supertraits, arena order, and graph rollback. Bounded/GAT
declarations and wrong arity/kind remain fail-closed. Ordered positional
supertrait arguments and associated equalities retain resolved trait and
associated-item DefIds, RHS types, exact spans, and deep ownership. This clears
`ops::Shr<u32, Output = Self>` in `num/dec2flt/float.rs:20`. Both probes now
agree that `pub trait Thin = Pointee<Metadata = ()> + PointeeSized;` lowers
with exact resolved RHS identity. Alias chains and alias use in predicates and
supertraits are retained without prematurely erasing the alias into its
bounds; canonical obligation expansion remains future solver work. Auto traits
retain authenticated identity, and safe itemless negative impls
retain full polarity, generics, predicates, types, identities, and spans;
duplicates and positive/negative overlap reject. Structural trait type defaults
recursively substitute `Self` and prior lifetime/type arguments with DAG
memoization, and explicit trait lifetime arguments retain authenticated static,
early-bound, or predicate-binder identity. Explicit const trait arguments
remain unsupported, so const-default substitution is not claimed as a
source-reachable feature. Associated-type trait bounds retain positional
lifetime/type arguments and Self-free trailing defaults in their named trait
type, with equality bindings kept separately. The next HIR rejection is the
lifetime associated-type bound in `type Unsigned: Copy + 'static` at
`intrinsics/fallback.rs:13`, item 1, span `502..509`.

Enum variant attributes are preserved in order with exact spans rather than
discarded. The graph-owned effective view evaluates cfg/cfg_attr before
publication and preserves the parent enum plus original AST variant index.
Enum-self, explicit variant, alias, glob, and checked-path imports use that
identity; named-field variants are type-only, while unit and tuple variants
also publish value constructors. Canonical HIR `hir-v30` pre-reserves a
distinct definition for every source variant, retains it on the variant
payload, binds it to the final enum item/index, and revalidates structural
imports when the enum becomes bound. The parent enum definition is never
substituted for a constructor.

Lifetime tokens inside the preceding `define_bignum!` transcriber retain their
delimiters instead of being misclassified as a long character literal. The
narrower source-fixture adapter parses `primitive_docs.rs` but rejects its
unsupported item set atomically.

Macro ASTs distinguish ordinary invocations, `macro_rules!` definitions, and
declarative macro definitions. The parser preserves both `pub macro name {
... }` and `pub macro name($tokens) { ... }` as opaque declarations, including
their attributes, visibility, optional parameter token tree, body, delimiter,
and source span. Rule-bodied declarations use the bounded `macro_rules!`
engine. Parameter-style declarations normally fail during planning. The one
ordinary admitted slice is a private, attribute-free, same-AST,
definition-before-use declaration called by an unqualified parenthesized
source item invocation in a module/root sequence. Its stored parameters and
body are treated as one synthetic rules arm. Public, imported, qualified,
forward, generated, cross-module, and associated-item uses remain unsupported.
Separately, the exact resolver-certified compiler builtin `cfg_select`
requires one bare `rustc_builtin_macro` attribute and expands only source item
invocations. The pinned Rust 1.90 `core/src/macros/mod.rs`, including all 19
such declarations, now passes the parser gate. Cfg-active named macro
declarations are retained
separately from semantic roots with their effective attributes, immediate
inline-module container, form, span, and generated provenance; cfg-disabled
declarations are absent.
The module graph registers each retained declaration in that exact module's
macro namespace with its source-qualified item identity. Definitions remain
absent from active and effective semantic item views. Crate-root
`macro_export`, child-to-parent `#[macro_use]`, declaration-ordered textual
scope, exact namespace fallback, public qualified local-crate paths, and
authenticated builtin include and cfg-select bindings are implemented.
Impl-member invocations reparse generated lists in an explicit impl context,
including specialization `default`; root reparsing rejects that qualifier.
Trait-body invocation planning and full root/trait/impl item-kind validation
remain outside the admitted grammar. Macro matching validates `ty` candidate
boundaries with the Rust type-fragment parser and restores both captures and
repetition indices across backtracking. Transcription admits the bounded
identifier forms `${concat(...)}`, `${ignore(...)}`, `${index()}`,
`${count(...)}`, and `$crate` with an explicit resolver-certified defining
crate; other metavariable expressions remain unsupported.
Unqualified `use`-imported invocation lookup remains outside the admitted
subset.

Each source patch records the upstream version, reason, checksum, and removal
condition. Patches must not replace runtime behavior merely to make a gate
green.

## Initial target contract

The first compiler provenance proof targets 32-bit x86 Linux with musl because
live-bootstrap has a proven self-hosted TinyCC 0.9.27 there. That proves the C
compiler executable, not yet a native Rust host: official Rust releases do not
ship an i686-musl rustc host, and upstream mrustc proves only x86-64 GNU. The
development host therefore retains x86-64 GNU for the reproducibility oracle.

Target-dependent behavior is isolated in a target descriptor and runtime
probe suite. Required probes include:

- integer widths and alignment;
- aggregate argument and return ABI;
- varargs;
- 64-bit arithmetic on i386;
- atomic operations used by `core` and `std`;
- thread-local storage;
- weak/linkonce symbol behavior;
- setjmp/longjmp and panic-abort shims;
- function/data section behavior;
- response files and maximum translation-unit size.

Atomics may be lowered to a correct lock-based fallback on the single initial
host. Replacing them with ordinary loads and stores is not permitted.

## Version policy

The oracle's highest proven direct target is Rust 1.90.0. As of 2026-08-07,
the latest stable release is Rust 1.97.1. The supported ladder is:

```text
cmrustc -> Rust 1.90.0 -> Rust 1.91.1 -> 1.92 -> 1.93 -> 1.94
        -> 1.95 -> 1.96 -> Rust 1.97.1
```

Exact patch releases and source hashes are pinned when each rung begins.
`cmrustc` is required to parse later language features only when an earlier
official compiler cannot make the next self-host transition.

“Latest” is frozen to the stable release named in the active milestone. A new
stable release after that milestone becomes a follow-up rung; it does not move
the completion line indefinitely.
