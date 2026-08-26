# Rust bootstrap roadmap (current measured target: 1.90)

The target is not permanently fixed at Rust 1.90.  The authoritative target
policy and current cross-pipeline audit are in `BOOTSTRAP_PARITY.md`: cmrustc
targets the newest release `T` demonstrably bootstrapped by the exact pinned
upstream mrustc revision, then hands off to official Rust release machinery.
The recovered proof currently measures `T = 1.90.0` and its first successor as
`S = 1.91.1`.

This is the development critical path from the audited 2026-08-23 tree to a
bootstrapped upstream Rust 1.90.0 compiler. `TASKS.md` remains the detailed
ledger; this document orders its tasks by the deepest consumable artifact.

## Audited baseline

- The audit began at `a6e7c309`. Commits through `1630867d` added declaration
  const terms, generic public functions, lossless public-function predicate
  capture, and canonical callable lifetime elision.
- The C implementation is a substantial fork.  Its general rustc-shaped CLI
  is still incomplete, but `--emit-cmrlib` and dependency-aware `--emit-c`
  now expose the exact bounded v3.2 executable profile.
- `make check-core-hir` is the strongest completed Rust-library gate.  On
  2026-08-26 the review-hardened current working tree reproduced the complete
  target-configured Rust 1.90 result twice: 363 sources and 451 modules with
  zero graph, import, or HIR errors, producing 38,176 items, 22,524 bodies,
  and 159,528 types.  Variant-aware library capture also succeeds with
  451 modules, 1,658 public type entries, and 20,747 public value entries.
- The next measured boundary contains 252 traits, 9,092 impls, and 9,854
  generic parameters, including 2,411 const and 1,443 lifetime parameters.
  Commits `fdfbe33c` through `a6e7c309` began extending library capture and
  declaration metadata for that surface.
- This audit extends declaration metadata through v2.6. Version 2.3 transports authenticated
  literal and parameter const uses in named generic arguments and array
  lengths, plus predicate-free public free functions with lifetime, type, and
  const parameters. Version 2.4 adds a bounded public-function predicate
  section and opaque reference-only trait/associated identities. Version 2.5
  preserves REQUIRED, CONST_IF_CONST, and CONST predicate modifiers as
  structural facts. Version 2.6 permits an opaque trait-alias identity as a
  direct predicate target without transporting its expansion. Strict decode
  tries exact v2.6, v2.5, v2.4, then v2.3 only on an unsupported-version
  result; legacy v1.0/v1.1 bytes remain unchanged.
- A deterministic object-bearing cmrlib now exists for the bounded G3
  provider/consumer profile.  No compiler-built `core.rlib`, `alloc`, `std`,
  `rustc`, or `cargo` artifact exists, so M6-06 remains the active vertical
  milestone.
- The current full-core declaration-v2.6 encoder still rejects atomically at
  the previously localized ITEM-family boundary and emits zero bytes.  This
  is the expected format boundary: v2.6 cannot transport real trait/alias
  declarations, associated children, impl headers, or their namespace
  identities.  The next G4 slice is therefore a complete v3 ordinary-trait
  declaration plus its canonical and reexported namespace bindings, not a
  v2.7 omission or another HIR census.
- The corrected value-aware `check-core-metadata` reaches the same clean HIR
  census after v2.3. Its first library-capture rejection was
  `core/src/contracts.rs:19`'s `build_check_ensures<Ret, C>`: its `C: Fn(&Ret)
  -> bool + Copy + 'static` clause lowers to two trait predicates and one
  outlives predicate. Library capture now owns and authenticates all three
  predicate families, including nested callable binders, trait arguments, and
  associated equalities. Declaration metadata v2.4 transports this exact
  scope-free required-predicate shape, its predicate-owned late-bound input
  region, the `FnOnce::Output = bool` equality, and `C: 'static` without
  publishing incomplete trait declarations.
- Predicate capture now also owns a canonical, reference-only nominal closure
  for every directly named trait, its supertraits, and associated types used
  by equalities. For the contracts frontier this retains `Fn`, `FnMut`,
  `FnOnce`, `Copy`, the true `FnOnce::Output` parent, and the exact
  `Fn`-to-`Output` availability witness. These records are opaque identities:
  they do not publish namespace entries or create empty trait declarations.
  Strict GCC, TinyCC, and Clang sanitizer library/metadata tests pass. Fresh
  consumers retain the opaque identities as RESERVED definitions and re-encode
  the constrained declaration byte-identically; trait solving and projection
  normalization remain deferred.
- An instrumented whole-core pass after `754cd5c4` proved the next capture
  frontier was `core::ptr::null<T: PointeeSized + Thin>`: `Thin` is a trait
  alias, not an ordinary trait. Reference-only closure capture now
  distinguishes trait aliases and retains `Thin`, `Pointee`, and
  `PointeeSized` identities. It deliberately does not turn Thin's internal
  `Pointee<Metadata = ()>` bound into a function-predicate equality or
  availability fact; alias-definition semantics remain deferred.
- At `1746e681`, an optimized whole-core gate completes value-aware library
  capture: 451 modules, 1,632 public type entries, and 20,692 public value
  entries. The previous v2.3 encoder then rejected unsupported HIR and emitted
  zero bytes. V2.4 subsequently crossed the first bounded predicate shape.
- At `4bd79751`, the post-v2.4 optimized gate reproduces that complete capture
  exactly and the v2.4 encoder still rejects fail-closed with zero bytes. The
  intervening capture regression was a validator bug: two predicate equality
  occurrences may share one canonical availability identity. Availability is
  now authenticated as a bidirectional deduplicated set. The next encoder
  slice must cover the first unsupported real-core predicate shape. An
  encoder-policy trace identifies function DefId `1:9087` with a scope-free
  `CONST_IF_CONST` (`~const`) predicate before the already known direct `Thin`
  trait-alias bound. The trace's `direct=0` is the authenticated ordinary-trait
  enum tag, not a missing reference; the modifier is the sole policy rejection.
- Declaration v2.5 transports that complete closed modifier enum using stable
  wire constants. Fresh decode restores the exact modifier and re-encodes
  byte-identically; v2.4 predicate records default to REQUIRED. Const-trait
  capability is not yet a transported trait fact, so solving and projection
  remain deferred. The post-v2.5 whole-core gate again proves zero HIR errors
  and complete 451/1,632/20,692 library capture, then returns `unsupported HIR`
  with zero metadata bytes. Source and encoder inspection identify the direct
  `Thin` trait-alias predicate as that policy boundary.
- Declaration v2.6 transports the direct `T: Thin` identity without changing
  the NREF/PRED payload layout. Thin remains an opaque RESERVED trait-alias
  identity: no alias body, `Metadata = ()` equality, availability fact, item,
  namespace binding, or solver evidence is synthesized. Exact v2.5 retags of
  alias-backed predicates reject atomically, while ordinary v2.5 artifacts
  continue to decode and upgrade byte-identically. The next whole-core probe
  re-measures the first unsupported declaration shape.
- At `48418fd0`, the optimized post-v2.6 gate again proves zero HIR errors and
  complete 451/1,632/20,692 library capture, then rejects metadata with zero
  bytes. Failure-only instrumentation localizes the rejection to ITEM
  collection. This is a format-family boundary, not one missing scalar:
  declaration v2 cannot encode core's complete trait, trait-alias, associated
  item, and 9,092-impl universe, nor publish trait namespace bindings. Those
  families must not be skipped or materialized as fake empty traits.
- The downstream-safe successor is the capability-manifested v3 family.  Its
  exact v3.2 executable-slice profile is implemented and consumed at runtime;
  the broader declaration-consumable checkpoint must still transport real
  trait/alias/associated declarations, projection and function-pointer types,
  impl headers, and complete namespace identities while keeping solver absence
  open. A later authenticated completeness layer may seal the cfg-active impl
  universe; bodies, macros, semantic attributes, and link inputs remain
  separately declared capabilities rather than implicit omissions.
  `docs/METADATA_V3.md` is the normative schema and acceptance contract.
- The first bounded v3.0 `LOWER_SAFE` declaration slice is implemented.  A
  lowered provider containing `Gate<T: ?Sized>`, `pub use Gate as
  GateReexport`, and `needs<X: Gate<u8>>` produces canonical 3.0 bytes with
  schema-1 family descriptors.  Decode/re-encode is byte-identical; an exact
  crate/target/layout/panic/cfg expectation is checked before HIR mutation;
  and a fresh HIR consumer lowers both `dep::Gate<u8>` and
  `GateReexport<u8>` to the same complete trait DefId.  The codec rejects
  orphan supporting types, aliases without defining namespace entries,
  malformed locals/ranges, wrong expectations, and late restore failures
  transactionally.  Full strict GCC and TinyCC suites plus focused Clang
  ASan/UBSan/LSan pass.  This slice covers no associated item, alias body,
  projection, impl, macro, semantic attribute, general body, or link input;
  it therefore does not make a core artifact or close M6-06.
- Bounded ordinary ITEM transport now also preserves a public top-level unit
  struct and the independent visibility of its value constructor.  Public
  type reexports no longer publish private, crate-only, or
  `#[non_exhaustive]` constructors, and fresh materialization retains the
  struct DefId without fabricating a function.  The current optimized core
  probe completes the zero-error 363-source/451-module HIR census and
  constructor-aware 451/1,632/20,721 library capture.  After explicit
  declaration-only projection of validated crate/module attributes, v3.0
  reached the public `LayoutErr` type-alias reexport at
  `core/src/alloc/mod.rs:19` and rejected it exactly with `stage=namespace`,
  `reason=binding-shape-unsupported`, and zero output.  Bounded v3.0 now
  transports that free alias, its zero-argument named-ADT target, private
  defining module, distinct alias/struct DefIds, and constructorless
  `LayoutError` through deterministic capture, decode/re-encode,
  materialization, and fresh-consumer normalization.  The next optimized core
  probe completes the same 451/1,632/20,721 library census and rejects the
  `ascii::Char` reexport at `core/src/ascii.rs:20`: its target `AsciiChar` is
  the first unsupported enum binding.  Ordinary enum/aggregate transport is
  therefore the next measured capability; associated declarations are not
  yet the immediate frontier.
- The v3.0 crate/module attribute projection above is valid only for its
  deliberately narrow declaration-name/DefId lookup consumer.  Its `ABSENT`
  family state means semantic-attribute records are unavailable, not that the
  source contained no attributes.  `alloc` must not consume this profile:
  before that gate, macros and semantic attributes require cfg-active
  completeness, a closed normalization/rejection policy, and authenticated
  source/configuration/dependency closure.
- Subsequent bounded v3.0 checkpoints crossed the `ascii::Char` enum and
  public enum-variant reexport boundaries, then added declaration-only CONST,
  default implicit diagnostic enums, and named aggregate/union transport.
  The committed `b78a18a8` producer/codec/materializer slice preserves
  `Assume`, `ManuallyDrop<T: ?Sized>`, and `MaybeUninit<T>` structural facts,
  ITEM-owned generics, field order/visibility/types, lang identities,
  transparent representation, and public aliases without inventing value
  constructors.  Strict GCC, TinyCC, and Clang ASan/UBSan/LSan focused gates
  and a fresh integrated strict run pass.
- The post-`b78a18a8` pinned-core probe measures the current v3.0 frontier as
  `CACHED_POW10` at `core/src/num/flt2dec/strategy/grisu.rs:29`:
  `stage=namespace`, `reason=binding-shape-unsupported`, `binding=value`,
  `ast_item=static`, `def=1:2845`.  Graph/HIR/library remain green at
  363/451 sources/modules, 38,176 items, 22,524 bodies, 159,528 types, and
  451/1,658/20,747 library entries.  The next bounded declaration step is a
  real STATIC plus ordered tuple and fixed-array types with an exact scalar
  `usize` length.  The table initializer and storage are not carried by v3.0
  and remain a later body/object/link gate.
- Commit `12c92627` transports that declaration-only STATIC plus ordered tuple
  and fixed-array types, exact scalar `usize` length, immutable/mutable static
  identity, and body-presence-without-body provenance.  Strict GCC, TinyCC,
  Clang ASan/UBSan/LSan, and a fresh integrated strict run pass.  Its pinned
  whole-core probe again reaches the zero-error 363-source/451-module HIR and
  451/1,658/20,747 library census, then measures the next v3.0 rejection at
  `core/src/prelude/mod.rs:21`: `stage=namespace`,
  `reason=binding-shape-unsupported`, `binding=enum-variant`,
  `namespace=type`, `ast_item=enum`, `def=1:22477`.  This is the TYPE half of
  tuple variant `Option::Some` introduced by the Rust 2015 prelude glob.  The
  next bounded declaration step is therefore generic Rust-default mixed
  UNIT/TUPLE enums, retained item/variant lang identities, tuple-field generic
  ownership, enum ADT applications, and exact TYPE/VALUE constructor twins.
- Commit `5d055423` transports generic Rust-default UNIT/TUPLE enums, retained
  item/variant lang identities, enum applications, exact flattened constructor
  twins, and recursive aggregate-field generic-owner validation.  The next
  pinned-core probe measures the definition-free primitive TYPE reexport at
  `core/src/primitive.rs:41`.  Commit `eb8281c1` transports the closed BOOL
  through F64 primitive namespace set and aliases without synthesizing a DefId,
  ITEM, VALUE mate, or TYPE record; strict GCC, TinyCC, and sanitizer gates plus
  fresh consumer lowering cover every admitted primitive.
- The post-`eb8281c1` pinned-core probe measures `#[rustfmt::skip]` on
  `core/src/char/mod.rs:28`'s `CharTryFromError` reexport (`def=1:19977`, source
  item `117:4`).  Commit `2bbd80f8` projects only the exact bare, depth-zero,
  source-authenticated public-reexport form and proves projection erasure,
  coexistence with `stable`, private-import exclusion, determinism, and atomic
  malformed/provenance rejection.  The following whole-core probe again
  reaches the complete 451/1,658/20,747 library census and measures the current
  frontier at `core/src/ffi/mod.rs:12`: `doc(inline)` on the `CStr` reexport
  (`def=1:20058`, source item `121:1`) fails with
  `stage=namespace`/`reexport-attribute-projection-unsupported`.  Namespace
  collection therefore remains the active gate; the later `Allocator` trait
  family is not yet a measured item frontier.
- Commit `659d29e4` closes the exact `doc(inline)` public-reexport projection.
  Its probe measures `doc(hidden)` on `core::hash::SipHasher13` at
  `core/src/hash/mod.rs:91` (`def=1:24504`, source item `203:2`), and commit
  `53ff35f2` closes that final source-censused reexport attribute without
  changing item admission.  The next pinned-core run proves namespace
  collection complete and enters item collection.  The current measured v3.0
  frontier is `core::alloc::Allocator` at `core/src/alloc/mod.rs:105`
  (`def=1:26860`, source item `252:21`, rejected item `229`), reported as
  `stage=items`/`item-source-invalid` because its retained `unstable` attribute
  reaches the source gate before unsafe safety and seven associated METHODs
  reach trait-shape validation.  A bounded associated-method declaration and
  library foundation is next; exact Allocator additionally requires complete
  `Layout`, `NonNull`, `Result`, `Sized`, and method-signature closure.
- Commit `c74680bf` provides the bounded associated-METHOD foundation without
  pretending it is exact Allocator: unsafe trait safety/ranges, source-ordered
  child methods, shared erased receivers, method safety/default promises,
  `Self: Marker`, associated library reachability/lookup, fresh restoration,
  and rollback are all green while old empty-AITM/free-function bytes remain
  exact.  Commits `310a7e1c` and `159686dd` close the authenticated generic
  ENUM-application/slice/raw-pointer capture and receiver-driven recursive
  output-elision gaps.  The clean pinned-core probe at `159686dd` still
  measures the same Allocator parent and `item-source-invalid`, but the exact
  remaining source fact is now source-ordinal-6 `by_ref`'s
  `#[inline(always)]`: the earlier six method signatures pass, while the
  associated-member attribute classifier rejects the inline hint before
  `by_ref`'s otherwise representable signature is emitted.  The immediate
  work is therefore a closed, byte-neutral associated-function inline
  projection.  Commit `5b5509f6` implements that exact associated-only
  projection; the following clean probe advances to `core::alloc::Layout` at
  `alloc/layout.rs:40` (`def=1:26881`, source item `252:4`, rejected item
  `11511`) with `stage=items`/`item-shape-unsupported`.  The active measured
  dependency is now `Layout -> Alignment -> private AlignmentEnum -> usize`,
  requiring tuple aggregates, wider explicit integer enum reprs, and
  authenticated transitively reachable private ITEMs without public namespace
  entries.  Commit `e79c819e`
  separately replaces repeated TYPE-arena rescans with memoized depth
  traversal, exact-key stable sorting, adjacent deduplication, and a
  multi-crate DefId index, satisfying the hostile resource bound without a
  wire change.  Commits `b4c065aa`, `e1eba5b9`, and `c5b4d093` complete the
  Layout descriptor, consumer, and producer slices.  A clean pinned-core run
  at `c5b4d093` keeps the zero-error 363-source/451-module graph, 38,176-item
  HIR, and 451/1,658/20,747 library census, and advances to
  `core/src/any.rs:113`: `Any` (`def=1:18858`, source item `99:3`, rejected
  item `110`) fails at `stage=items`/`item-source-invalid`.  The active exact
  facts are the retained `rustc_diagnostic_item = "Any"` identity,
  `Self: 'static`, and a required safe shared-receiver associated method.
  `TypeId` remains a following structural dependency rather than permission
  to fabricate an opaque return type.  Commits `0f7b9fb7`, `c0189406`, and
  `0256f506` close the exact Any declaration path.  The next clean whole-core
  run measures `TypeId` itself at `core/src/any.rs:711` (`def=1:18886`, source
  item `99:30`, rejected item `10800`) with
  `stage=items`/`item-shape-unsupported`.  Its single `pub(crate)` field is an
  array of immutable unit raw pointers whose length is the target-dependent
  result of `16 / size_of::<*const ()>()`; the next slice must carry exact
  crate visibility and configured-target evaluation, not host `sizeof`.
  Commits `2b19ef47` and `592d17a1` complete that descriptor and the
  target-aware producer/consumer path.  The clean probe at `592d17a1`
  preserves the exact zero-error graph/HIR and 451/1,658/20,747 library
  census, then measures `core/src/any.rs:856`'s public `type_name` function
  (`def=1:18898`, source item `99:42`, rejected item `10801`) at
  `stage=items`/`item-source-invalid`.  Commits `1f8f6d22`, `1447e225`, and
  `a58142db` complete that exact const declaration path without claiming its
  body is executable.  The next clean run measures
  `core/src/any.rs:896`'s `type_name_of_val` (`def=1:18899`, source item
  `99:43`, rejected item `10802`) at the same stage/reason.  Its omitted input
  lifetime is now normalized only at its exact source-authenticated input
  boundary and transported under contextual erased-root validation by commits
  `bef6bc8a`, `c03b3378`, and `b0a41eef`.  The clean pinned-core probe at
  `b0a41eef` preserves the zero-error graph/HIR and exact
  451-module/1,658-type/20,747-value library census, then measures
  `core/src/arch.rs:76`'s public `breakpoint` function (`def=1:36071`, source
  item `332:5`, rejected item `19448`) at
  `stage=items`/`item-source-invalid`.  Commits `9b7740c7`, `19635436`, and
  `cd76600e` complete that exact zero-generic unit declaration profile.  The
  clean pinned-core probe at `cd76600e` keeps the zero-error graph/HIR and
  exact 451-module/1,658-type/20,747-value library census, then measures
  `core/src/array/iter.rs:20`'s `IntoIter` (`def=1:19103`, source item
  `100:18`, rejected item `10819`) at
  `stage=items`/`item-shape-unsupported`.
  Commit `112049fe` completes its exact const-generic declaration and
  non-public structural dependency closure without claiming impl, drop-glue,
  or executable-body authority.  The clean pinned-core probe at `112049fe`
  keeps the zero-error graph/HIR and exact
  451-module/1,658-type/20,747-value library census, then measures
  `core/src/array/mod.rs:181`'s `TryFromSliceError` (`def=1:18905`, source item
  `100:24`, rejected item `10808`) at
  `stage=items`/`item-shape-unsupported`.
  Commit `ba215beb` completes that exact zero-generic Rust-repr tuple-error
  producer.  The clean pinned-core probe at `ba215beb` retains the zero-error
  graph/HIR and exact 451-module/1,658-type/20,747-value library census, then
  measures `core/src/array/mod.rs:108`'s `from_fn` (`def=1:18901`, source item
  `100:20`, rejected item `10804`) at
  `stage=items`/`item-source-invalid`.
- Commit `68e338e9` transports `from_fn`'s exact `T, const N: usize, F`
  signature, `[T; N]` return, `FnMut<(usize,), Output = T>` predicate, and
  complete `Tuple`/`FnOnce`/`FnMut` declaration closure without body or MIR
  authority. Commit `62fd1289` removes the false same-crate restriction on
  otherwise fully authenticated associated equalities. The clean pinned-core
  probe at `62fd1289` retains the zero-error graph/HIR and exact
  451-module/1,658-type/20,747-value library census, then measures
  `core/src/array/mod.rs:173`'s `from_mut` (`def=1:18904`, source item
  `100:23`, rejected item `10807`) at
  `stage=items`/`item-source-invalid`.
- Commit `292adaf1` adds the exact source-authenticated paired lifetime
  normalization and declaration-only `from_mut` profile. The clean pinned-core
  probe at `292adaf1` retains the zero-error graph/HIR and exact
  451-module/1,658-type/20,747-value library census, then measures its shared
  twin `core/src/array/mod.rs:165`'s `from_ref` (`def=1:18903`, source item
  `100:22`, rejected item `10806`) at
  `stage=items`/`item-source-invalid`.
- Commit `fbe4cee4` extends the same exact paired boundary to shared/shared
  roots without admitting mixed mutability. The clean pinned-core probe at
  `fbe4cee4` preserves zero graph/import/HIR errors, 38,176 HIR items, and the
  exact 451-module/1,658-type/20,747-value library census. It clears both array
  reference helpers and measures the next v3.0 frontier at
  `core/src/array/mod.rs:54`: `repeat<T: Clone, const N: usize>`
  (`def=1:18900`, source item `100:19`, rejected item `10803`) at
  `stage=items`/`item-source-invalid`. The next capability must retain the
  exact `T: Clone` declaration predicate and const-generic array result while
  keeping the generic inline body unavailable.
- Parenthesized callable-trait input elision is normalized before metadata:
  omitted input lifetimes become deterministic predicate-owned late-bound
  parameters, and an elided output inherits the sole distinct input lifetime.
  Ambiguous outputs and unrepresentable mixed binders reject rather than
  publishing inference variables across a crate boundary.
- The working directory contains roughly 17 GiB of untracked experimental
  build directories. They are evidence/debug debris, not source; do not use
  their presence as a passing gate or delete them without a separate cleanup
  decision.

The evidence came from Git and the current tree, the August 7-15 Codex
sessions, and the August 21-22 Claude continuation. Historical green tests are
useful context but must be rerun at the current revision before release claims.

## Critical path

The later parity audit changes the sequencing below: implement only enough P1
metadata to close P2's executable cross-crate canary, then broaden the same
artifact toward `core`.  Do not complete a whole-`core` declaration format
before any downstream stage consumes it.

### P1: Minimum consumable declaration slice (bounded G3 profile complete)

1. Implement the smallest v3 capability set needed to transport a public
   generic function, a trait, and one impl without omitting facts that affect
   that fixture. The gate must fail on unsupported HIR and require nonempty
   bytes.
2. Decode it in a fresh HIR context, re-encode it byte-identically, and feed it
   immediately into P2. Preserve deterministic local handles, atomic decode
   rollback, and fail-closed version and capability checks.
3. Put the checked metadata in the deterministic SysV archive container and
   teach the driver to load that container. Call it an `.rlib` only when the
   archive contract and any required object members are explicit.

Acceptance: two isolated producers emit identical nonempty fixture artifacts;
a fresh process loads one without producer state, and rejects corrupt or
semantically incomplete artifacts atomically. P2, not bytes alone, closes P1.

The 2026-08-25 exact v3.2 gate closes this bounded P1 profile with a canonical
configuration/source identity, trait and positive primitive impl records,
one generic `RETURN_ARGUMENT` body recipe, a link manifest, and a checked
native object member.  This does not close M6-06 for real `core`.

### P2: One executable cross-crate canary (bounded G3 profile complete)

Carry one generic public function plus one trait implementation across the
artifact boundary through body typing, trait selection, MIR, instance
reachability, layout, C emission, object production, archive loading, linking,
and execution. This vertical canary comes before broadening declaration-only
coverage that no later stage consumes.

Acceptance: GCC and TinyCC development builds produce the same observable
result; unsupported substitutions, missing impl facts, and stale artifacts
hard-fail without preserving a newly written output.

`tests/codegen/run_g3_cross_crate.sh` now passes this acceptance from both
strict-GCC-built and TinyCC-built cmrustc binaries, with Clang sanitizer
coverage.  The canary is intentionally `no_core`; M6-08 remains open until a
real core/alloc-linked executable runs.

After this passes, broaden the same capability-manifested artifact along the
first real-`core` failure: ordinary items, values, and types; traits and
aliases; associated declarations and projections; impl completeness;
dependency identities, macros, semantic
attributes, generic bodies/const IR, link inputs, and object members. A real
`core.rlib` requires a fresh linked consumer; a complete metadata census is
only an intermediate diagnostic.

### P3: Build `alloc` against `core` (M6-07)

Use the decoded core macro and semantic artifacts while extending only the
features reached by the first alloc failure. Close the required const
evaluation, coercion/unsizing, lang-item, intrinsic, allocation, panic, and
link dependencies as executable vertical slices.

Acceptance: alloc emits a nonempty deterministic artifact, a fresh dependent
compile loads both core and alloc, and a small allocation probe links and runs.

### P4: Rust 1.90 crate orchestration (M6-01 through M6-04 and M6-09)

Implement the C `hcargo` path using the exact Rust 1.90 manifests and flags:
manifest/cfg parsing, deterministic DAG scheduling, artifact caching, build
scripts and overrides, host/target separation, proc macros, and native link
inputs. Start with the core/alloc subgraph, then compiler_builtins,
proc_macro, std, and test before the compiler graph.

Acceptance: serial and parallel clean builds select the same pinned DAG and
produce checked nonempty artifacts without Python/CMake/C++ being required by
cmrustc or hcargo themselves.

### P5: Build and self-host upstream rustc 1.90 (M7-01 and M7-02)

Obtain and checksum the complete Rust 1.90.0 source archive; the current local
cache is library-only. Build the patched upstream compiler and Cargo graph,
then require version output, hello-world compilation/execution, a Cargo
workspace, and a compiler-built-compiler comparison. A successful process
with no artifact is never sufficient.

Acceptance: the cmrustc-produced Rust 1.90 toolchain rebuilds the pinned Rust
1.90 sources and the normalized stage comparison passes. The resulting rustc
and Cargo compile representative source without cmrustc at runtime.

### P6: Provenance and later release ladder (M8, then remaining M7)

After the development bootstrap works, insert cmrustc/hcargo into the pinned
i386-musl live-bootstrap chain, resolve the Rust host transition, and repeat
the complete artifact and self-host gates under the source-only closure. The
1.91.1-to-current official release ladder is downstream of the requested
1.90 self-host and must not distract from P1-P5.

## How to lean on upstream mrustc

- Use the retained C++ compiler as an oracle for AST/HIR/MIR dumps, layout,
  symbols, crate flags, build-script outputs, and runtime behavior.
- Reuse its pinned Rust patches, crate ordering knowledge, bootstrap scripts,
  and minimized failing inputs where their provenance is understood.
- Differentially compare exact fixtures and artifacts, but do not import a
  host-built mrustc binary into the final trust chain.
- Prefer the first failing real Rust 1.90 construct, reduce it to a focused
  positive/negative test, implement the exact semantics, run strict/TinyCC and
  sanitizer gates, then rerun the real frontier.

## Verification cadence

- Per change: focused unit/process tests, strict C99/Werror build, and TinyCC
  build for every touched compiler path.
- Per frontier checkpoint: `check-core-hir`, `check-core-metadata`, fresh
  producer/consumer process tests, and the deepest executable canary.
- Per milestone: clean isolated twin builds with hashes, corruption/rollback
  negatives, ASan/UBSan/LSan on the affected slice, and exact producer/source
  revision recording.

Do not advance a milestone from parser, census, or zero-error process output
alone. The authority is always the deepest nonempty artifact consumed by a
fresh later stage and executed where applicable.
