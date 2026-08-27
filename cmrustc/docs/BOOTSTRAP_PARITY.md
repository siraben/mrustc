# cmrustc bootstrap parity map

This document charts the shortest evidence-backed path from the current C
compiler to the point where it can replace the retained C++ `mrustc` in the
Rust bootstrap.  It is about artifacts that a later stage can consume, not
about how many source declarations a front end can count.

## Target policy

Let `T` be the newest Rust release that an exact, pinned upstream `mrustc`
revision demonstrably bootstraps with a recorded source archive, patch set,
build overrides, target triple, and clean acceptance run.  Let `S` be the
first official Rust release built by that `T` toolchain.

The currently recovered proof fixes `T = 1.90.0` and `S = 1.91.1`.  Those are
measurements, not permanent policy.  A newer upstream proof moves `T` without
changing this roadmap.  cmrustc only needs to build `T`; official rustc
self-host and release-ladder machinery owns `S` through latest-at-run-time.

The successful Nix ripgrep experiment does not prove this cmrustc edge.  Its
first compiler is `${mrustc}/bin/mrustc` from `thepowersgang/mrustc`, the C++
implementation.  It proves the downstream mechanism through the releases in
that recovered run once a real `T` toolchain exists.

More precisely, the recovered branch at `50dc282b0c34` proves the source-only
sequence 1.90.0 -> 1.91.1 -> 1.92.0 -> 1.93.1 -> 1.94.0 -> 1.95.0 and then
ripgrep.  It does not yet prove its documented 1.96/1.97 continuation.  “To
latest” below is the policy and remaining gate, not a claim about that archived
run.  Its 1.93.1 source also disagrees with the 1.93.0 entry in `PINS.md`; the
ladder record must settle that version and use one canonical archive format
and hash before it is provenance authority.

## Audited boundary

The current tree has four important, but different, kinds of evidence:

1. Strict GCC, Clang, and TinyCC tests exercise a real bounded `no_core`
   source-to-C-to-executable path.
2. Private cmhir v1/v2 fixtures prove deterministic, fresh-process transport
   for a limited declaration subset.  The first bounded v3.0 `LOWER_SAFE`
   slice now additionally captures a real generic ordinary trait, its
   canonical and reexported namespace names, and a public generic value whose
   predicate names that same trait identity.  Decoded bytes materialize in a
   fresh HIR context and a new consumer lowers both the direct and reexported
   trait paths to the same bound DefId.
3. The bounded G3 profile emits a deterministic object-bearing cmrlib from an
   admitted provider, then a fresh process loads its trait, positive primitive
   impl, and generic `RETURN_ARGUMENT` body recipe and emits a linked consumer
   that executes under both GCC and TinyCC.
4. The review-hardened current tree passes the target-configured Rust 1.90
   `core` front end: 363 sources, 451 modules, 38,176 HIR items, 22,524 body
   records, and 159,528 types, with zero graph, import, or HIR errors.  Its
   value-aware library capture contains 451 modules, 1,658 public type
   entries, and 20,747 public value entries.  Body records are not equivalent
   to typed, executable bodies.

The 2026-08-25 current-source repair implements bounded implicit-negative
coherence reasoning rather than source-specific exemptions.  After header
unification it may prove a local trait predicate impossible only when the
subject and every trait type input are orphan-closed, the complete local
positive-provider set has no viable candidate, and recursive provider
requirements are likewise impossible.  It fails open for foreign/open,
fundamental-wrapper, compiler-only, cyclic, or exhausted cases.  This crossed
the `error.rs`, array/`TryFrom`, iterator, and callable `MultiCharEq` frontiers;
the full strict GCC and TinyCC suites, Clang ASan/UBSan/LSan, and two complete
current-source core probes pass.

The active G4 boundary is now declaration metadata, not G1.  Full library
capture succeeds, but v2.6 rejects at its previously localized ITEM family and
emits zero bytes because that format cannot represent complete traits,
aliases, associated children, impl headers, or trait namespace entries.  The
bounded v3.0 `LOWER_SAFE` path now deterministically transports the original
ordinary-trait/value fixture plus public unit structs and constructors, free
aliases, zero-argument named ADTs, explicit and implicit unit enums, associated
enum-variant namespace aliases, declaration-only free constants, structural
slice/reference/pointer/application types, and the measured named-aggregate
profiles for `Assume`, `ManuallyDrop<T: ?Sized>`, and `MaybeUninit<T>`.  These
profiles now also include declaration-only free statics with ordered tuple and
fixed-array types plus an exact scalar `usize` array length.  These records
survive decode/re-encode, transactional materialization, library
restore, and focused fresh-consumer lowering without fabricating constructors,
bodies, initializer/storage values, or semantic completeness.

At commit `12c92627`, the fresh Rust 1.90 whole-core probe again reports zero
graph/import/HIR errors and the exact 451-module/1,658-type/20,747-value library
census.  V3.0 now crosses `CACHED_POW10` and reaches the next measured
namespace frontier at `core/src/prelude/mod.rs:21`: the Rust 2015 prelude glob
introduces the TYPE half of tuple variant `Option::Some` (`def=1:22477`), which
is rejected with `stage=namespace`, `reason=binding-shape-unsupported`,
`binding=enum-variant`, and `ast_item=enum`.  The next bounded dependency is a
generic Rust-default enum with mixed UNIT/TUPLE variants, retained item/variant
lang identities, scoped tuple fields, generic enum applications, and exact
TYPE/VALUE variant twins.  Initializer/storage/CTFE authority remains a
separate executable family.  Associated declarations,
projections/function pointers, full ordinary aggregates, cfg-complete impl
headers, macros, and complete semantic attributes remain later boundaries.
These are bounded fixture profiles, not whole-core v3.

Commit `5d055423` closes the generic Option/Result-like UNIT/TUPLE enum slice,
including retained variant lang identities and recursive field-generic scope.
Commit `eb8281c1` then transports exact definition-free primitive TYPE
reexports and aliases.  On that committed code, the whole-core probe remains
green through graph, HIR, and the 451-module/1,658-type/20,747-value library
census before measuring `#[rustfmt::skip]` at
`core/src/char/mod.rs:28`.  Commit `2bbd80f8` adds only that authenticated
reexport projection and retains byte-neutral projection semantics.  Its next
whole-core probe measures the current frontier at `core/src/ffi/mod.rs:12`:
`doc(inline)` on the public `CStr` reexport (`def=1:20058`, source item `121:1`)
is still rejected during namespace projection.  No declaration metadata bytes,
object-bearing `core.rlib`, or bootstrap-completion claim follows from these
attribute gates.

Commit `659d29e4` closes `doc(inline)` reexport projection.  Its whole-core
probe measures `doc(hidden)` on `core::hash::SipHasher13`; commit `53ff35f2`
then closes that last source-censused public-reexport attribute kind.  The next
probe again reports zero graph/import/HIR errors and the exact
451-module/1,658-type/20,747-value library census, proves namespace capture
complete, and measures the current item frontier at
`core/src/alloc/mod.rs:105`: `Allocator` (`def=1:26860`, source item `252:21`,
rejected item `229`) fails with `stage=items` and `reason=item-source-invalid`.
This is an unsafe, unstable trait with seven associated methods.  The next
staged capability is authenticated associated-method declaration/library
transport; it does not yet provide the full nominal/signature closure or an
object-bearing `core.rlib`.

Commit `c74680bf` adds a real but bounded unsafe-trait METHOD declaration
surface: exact trait-child identities, receivers, safety/default-body promises,
`Self`, erased references, method predicates, associated library lookup, fresh
materialization, and fail-closed unavailable bodies.  Legacy zero-AITM and
free-function representations remain exact.  Commits `310a7e1c` and
`159686dd` then add authenticated generic ENUM applications, slices/raw
pointers, and receiver-driven output-lifetime elision.  A clean pinned-core
probe at `159686dd` again reports zero graph/import/HIR errors, 38,176 HIR
items, and the exact 451-module/1,658-type/20,747-value library census before
measuring Allocator at the same parent item/reason.  The residual source fact
is now `Allocator::by_ref`'s `#[inline(always)]`: associated children are
walked in source order, the prior six signatures pass, and the unclassified
inline hint bubbles up as trait-level `item-source-invalid`.  This is a
byte-neutral capture projection gate.  Commit `5b5509f6` closes it for the
exact `inline`, `inline(always)`, and `inline(never)` forms on associated
functions only.  Its clean pinned-core probe advances to
`core/src/alloc/layout.rs:40`: `Layout` (`def=1:26881`, source item `252:4`,
rejected item `11511`) fails with `stage=items` and
`reason=item-shape-unsupported`.  The measured structural closure is the
public private-field `Layout` struct, transparent tuple `Alignment`, and its
private cfg-selected repr(u64) `AlignmentEnum`; no declaration metadata or
object artifact succeeds yet.  Commit `e79c819e` independently makes reachable
declaration TYPE canonicalization bounded and deterministic in multi-crate
hostile inputs; it does not broaden the semantic frontier.  Commits
`b4c065aa`, `e1eba5b9`, and `c5b4d093` then encode, materialize, and capture
that complete private dependency chain without publishing `AlignmentEnum`.
The clean pinned Rust 1.90 probe at `c5b4d093` again reports zero graph/import/
HIR errors, 38,176 HIR items, and the exact 451-module/1,658-type/20,747-value
library census.  It advances the measured v3.0 frontier to
`core/src/any.rs:113`: safe trait `Any` (`def=1:18858`, source item `99:3`,
rejected item `110`) fails at `stage=items` with `reason=item-source-invalid`.
The next declaration slice must retain its diagnostic identity, exact
`Self: 'static` outlives requirement, and safe-parent associated `type_id`
method; none may be projected into an unsafe or childless stand-in.  Commits
`0f7b9fb7`, `c0189406`, and `0256f506` implement that descriptor, consumer,
and producer path.  The following clean pinned-core probe advances to
`core/src/any.rs:711`: `TypeId` (`def=1:18886`, source item `99:30`, rejected
item `10800`) fails at `stage=items` with `reason=item-shape-unsupported`.
Its exact declaration requires a retained crate-visible field containing
`[*const (); 16 / size_of::<*const ()>()]`; the active work must preserve
crate visibility and bind the evaluated array length to the configured target
pointer width rather than the bootstrap host width.  Commits `2b19ef47` and
`592d17a1` encode that visibility and implement target-aware lowering,
capture, and materialization.  The clean pinned-core probe at `592d17a1`
again reports zero graph/import/HIR errors, 38,176 HIR items, and the exact
451-module/1,658-type/20,747-value library census.  It advances the measured
v3.0 frontier to `core/src/any.rs:856`: `type_name` (`def=1:18898`, source
item `99:42`, rejected item `10801`) fails at `stage=items` with
`reason=item-source-invalid` as a public VALUE/FUNCTION declaration.  Commits
`1f8f6d22`, `1447e225`, and `a58142db` encode, materialize, and capture its
exact const declaration while keeping its body unavailable.  The following
clean pinned-core probe preserves the same zero-error graph/HIR and exact
451-module/1,658-type/20,747-value library census, then advances to
`core/src/any.rs:896`: `type_name_of_val` (`def=1:18899`, source item `99:43`,
rejected item `10802`) at `stage=items` with
`reason=item-source-invalid`.  Commits `bef6bc8a`, `c03b3378`, and `b0a41eef`
constrain erased declaration roots, perform the exact source-authenticated
input-only lifetime normalization, and capture that declaration without
claiming executable body authority.  The clean pinned-core probe at
`b0a41eef` again reports zero graph/import/HIR errors, 38,176 HIR items, and
the exact 451-module/1,658-type/20,747-value library census.  It advances the
measured v3.0 frontier to `core/src/arch.rs:76`: public function `breakpoint`
(`def=1:36071`, source item `332:5`, rejected item `19448`) fails at
`stage=items` with `reason=item-source-invalid`.  Commits `9b7740c7`,
`19635436`, and `cd76600e` encode, materialize, and capture its exact
zero-generic unit declaration while leaving the inline intrinsic body
unavailable.  The clean pinned-core probe at `cd76600e` preserves the same
zero-error graph/HIR and exact 451-module/1,658-type/20,747-value library
census, then advances the measured frontier to `core/src/array/iter.rs:20`:
`IntoIter` (`def=1:19103`, source item `100:18`, rejected item `10819`) fails
at `stage=items` with `reason=item-shape-unsupported`.
Commit `112049fe` captures and freshly materializes its exact const-generic
declaration plus the reachable non-public `PolymorphicIter`, `IndexRange`, and
`PartialDrop` structural closure; it does not claim impl, drop-glue, or body
authority.  The clean pinned-core probe at `112049fe` retains the same
zero-error graph/HIR and 451-module/1,658-type/20,747-value library census,
then advances to `core/src/array/mod.rs:181`: `TryFromSliceError`
(`def=1:18905`, source item `100:24`, rejected item `10808`) fails at
`stage=items` with `reason=item-shape-unsupported`.
Commit `ba215beb` captures that exact zero-generic Rust-repr tuple-error
declaration.  The clean pinned-core probe at `ba215beb` again keeps the
zero-error graph/HIR and 451-module/1,658-type/20,747-value library census,
then advances to `core/src/array/mod.rs:108`: `from_fn` (`def=1:18901`, source
item `100:20`, rejected item `10804`) fails at `stage=items` with
`reason=item-source-invalid`.
Commit `68e338e9` captures and freshly materializes its exact const-generic
signature plus the complete reachable `Tuple`/`FnOnce`/`FnMut` callable-trait
declaration closure while keeping its generic body unavailable. Commit
`62fd1289` permits the resulting authenticated inherited associated equality
to be restated across a crate boundary. The clean pinned-core probe at
`62fd1289` again reports zero graph/import/HIR errors and the exact
451-module/1,658-type/20,747-value library census, then advances to
`core/src/array/mod.rs:173`: `from_mut` (`def=1:18904`, source item `100:23`,
rejected item `10807`) at `stage=items` with
`reason=item-source-invalid`.
Commit `292adaf1` source-authenticates its sole omitted input/output lifetime
relation, transports the exact mutable paired declaration, and leaves its body
unavailable. The clean pinned-core probe at `292adaf1` preserves the same
zero-error census and exact library totals, then advances to the shared twin
at `core/src/array/mod.rs:165`: `from_ref` (`def=1:18903`, source item
`100:22`, rejected item `10806`) at `stage=items` with
`reason=item-source-invalid`.
Commit `fbe4cee4` admits the exact shared/shared twin while retaining the
mutable/mutable profile and rejecting mixed roots. Its clean pinned-core probe
again reports zero graph/import/HIR errors, 38,176 HIR items, and the exact
451-module/1,658-type/20,747-value library census. Both reference helpers now
clear, and the measured v3.0 frontier advances to
`core/src/array/mod.rs:54`: `repeat<T: Clone, const N: usize>`
(`def=1:18900`, source item `100:19`, rejected item `10803`) at
`stage=items`/`item-source-invalid`. This is a declaration-only generic
function boundary: its callable body is not evidence until executable generic
body transport exists.
Commit `9c8f6439` captures and freshly materializes that exact declaration and
the complete reachable `Clone`/`Sized`/`MetaSized`/`PointeeSized`/`Destruct`
declaration closure, preserving compiler flags and predicate modifiers while
leaving imported bodies unavailable. Its clean pinned Rust 1.90 probe again
reports zero graph/import/HIR errors, 38,176 HIR items, and the exact
451-module/1,658-type/20,747-value library census. The measured v3.0 frontier
now advances to `core/src/array/mod.rs:146`: `try_from_fn<R, const N: usize,
F>` (`def=1:18902`, source item `100:21`, rejected item `10805`) at
`stage=items`/`item-source-invalid`. The next declaration capability must
retain the exact `Try`/`Residual` projection and associated-equality closure
used by `ChangeOutputType<R, [R::Output; N]>`; it must not treat the generic
inline body as transported executable authority.
Commit `574fda73` captured that declaration in a single-module fixture only;
commit `ebdcf4bc` collects public traits before values so the real cross-module
`core::array`/`core::ops` layout authenticates. The clean pinned Rust 1.90 probe at `ebdcf4bc` again reports zero graph, import,
and HIR errors, 38,176 HIR items, and the exact 451-module/1,658-type/20,747-value
library census. It advances the v3.0 frontier to `core/src/array/iter.rs:356`:
`pub trait NonDrop {}` (`def=1:19141`, source item `104:48`, rejected item
`113`) at `stage=items`/`item-source-invalid`. Because traits are now collected
first, this is the first public trait in the whole crate outside the admitted
trait declaration profile; its attributes are `#[doc(hidden)]`,
`#[unstable(issue = "none", feature = "std_internals")]`, and
`#[rustc_unsafe_specialization_marker]`, and the next trait slice must
authenticate that exact declaration or report the precise rejected fact.

There is still no compiler-built `core.rlib`, `alloc`, `std`, `rustc`, or
`cargo`, and there is no C `hcargo`.  The implemented `--emit-cmrlib` and
`--extern-cmrlib` path is deliberately the exact v3.2 executable-slice
profile, not a general Rust rlib or rustc-compatible driver.  It transports
one structural marker-trait/primitive-impl/generic-identity recipe family and
one native object; every unsupported live fact fails closed.

## Gap chart

| Stage | Current cmrustc boundary | Boundary required to replace mrustc |
| --- | --- | --- |
| Parse and expand | Whole-`core` declaration graph; bounded builtins | Body-position expansion, derives, dependency macros, and proc-macro execution for the `T` corpus |
| Resolve and HIR | Local imports and declaration census | Cross-crate identities, namespaces, reexports, traits, associated items, impl completeness, lang items, and semantic attributes |
| Bodies and type checking | Small `no_core` expression and trait slice | All body forms reached by `core`/`alloc`; coercions, projections, method selection, CTFE, closures, coroutines, vtables, and erased types |
| MIR | Assign, call, goto, return, and boolean switch over a small type set | General places, rvalues, control flow, drops, cleanup/unwind, casts, aggregates, statics, consts, intrinsics, and validation |
| Reachability and monomorphization | Local roots and a bounded substitution canary | Arbitrary type/lifetime/const substitutions across crates, trait/default methods, drop glue, vtables, statics, and stable symbols |
| Layout and ABI | Integers, references, and simple local structs | Enums/niches, unions, tuples, arrays, slices/DSTs, fat pointers, `repr`, SIMD, function ABIs, panic/unwind, atomics, and TLS |
| C and native output | C text plus one isolated, fixed-name native object for the exact G3 profile | General object/library production, dependency extraction, and linking for all required crate types and native inputs |
| Cross-crate artifact | Exact authenticated v3.2 marker-trait, primitive-impl, generic identity-recipe, link-manifest, and object slice | Complete declarations, impl facts, macros, generic body/const recipes, link inputs, objects, and archive symbol index |
| Orchestration | No C implementation | rustc-compatible invocation plus a C manifest DAG with build scripts, overrides, proc macros, host/target split, caching, and native dependencies |

The retained C++ implementation is an oracle for flags, crate ordering,
patches, AST/HIR/MIR, target layout, emitted symbols, and runtime behavior.  It
is not an acceptable binary input to the final cmrustc trust edge.

## Dependency-ordered gates

### G0: Lock the oracle and target

Record the exact upstream `mrustc` revision, `T` archive, patches, overrides,
target, and output identities.  Replay its `TestRustcBootstrap`-equivalent in
isolated inputs and record the `S` comparison.

Acceptance: one machine-readable record derives `T` and `S`; no roadmap text
or package expression silently selects a different release.

### G1: Keep the current front end green

Repeat strict/TinyCC twin builds, the native ABI suite, and the target-configured
whole-`core` HIR gate at current HEAD.  Historical census commits remain useful
diagnostics but do not make a changed compiler green.

Acceptance: isolated current-source runs agree and the full configured `core`
front end has zero graph, import, lowering, and semantic errors.

### G2: Capture the invocation contract

Record or translate the upstream rustc/minicargo contract: crate name/type,
target, extern/search paths, cfg/features, emit/output paths, metadata identity,
panic/codegen flags, environment, source patches, and native inputs.  A
manifest DAG and build-script override scaffold may proceed in parallel.

Acceptance: a trace comparison accounts for every input affecting a small
upstream crate build; unknown correctness-relevant flags fail closed.

The minimum compiler command surface is positional source, `-o`, crate name,
crate type (`rlib`, `bin`, `proc-macro`, or `dylib`), crate tag, `--extern`,
`-L`, `-l`, cfgs, edition, target, optimization/debug flags, depfile, panic
mode, and deferred build-command output.  It must also publish target cfgs.
Expected outputs follow upstream minicargo naming: `libNAME-TAG.rlib`, the
requested binary, or an executable `libNAME-TAG-plugin` proc macro.

### G3: Produce the smallest executable cross-crate artifact (complete)

Implement only the v3 capability families, body/const recipe, object/archive,
dependency loading, and linking needed for one public generic function and one
trait implementation.  This gate deliberately precedes broad declaration-only
metadata work.

Acceptance: a fresh process consumes the artifact, GCC and TinyCC link and run
the same result, two producers agree, and corrupt, incomplete, mismatched, or
stale inputs publish no output.

The 2026-08-25 gate meets this bounded acceptance with
`tests/codegen/run_g3_cross_crate.sh`: two isolated physical roots produce
byte-identical archives for each selected C compiler; the archive contains
authenticated `cmrustc.rmeta` and `cmrustc.object` members; the provider source
is removed before the consumer process starts; and the consumer resolves and
instantiates the transported generic only after its transported marker impl is
selected.  GCC and TinyCC link and run identical identity/object canaries.
Corrupt, truncated, missing, wrong-extern, wrong-target, wrong-source-digest,
and stale-identity cases are covered by the process gate plus focused exact
codec expectation tests, with existing outputs preserved.  This is G3 only:
the consumer C emitter handles the authenticated `RETURN_ARGUMENT` shape and
does not imply general cross-crate MIR or `core.rlib` support.

### G4: Produce a consumable `core` rlib

Broaden G3 along the first failing real-`core` path until the artifact carries
the complete cfg-active public declarations, macros and reexports, semantic
attributes, impl universe, generic bodies/const IR, link inputs, and necessary
objects.  Generalize type checking, MIR, monomorphization, layout, ABI, and C
emission as those failures require.

Acceptance: two isolated builds emit deterministic nonempty object-bearing
artifacts; a fresh dependent compile loads the archive and linked representative
`no_std` probes execute.  Metadata bytes or an HIR census alone cannot pass.

### G5: Build `alloc`

Build `alloc` against the G4 artifact.  Close const evaluation,
coercion/unsizing, lang-item, intrinsic, panic, allocation, and wider MIR gaps
as executable vertical slices.

Acceptance: `alloc` emits a consumable artifact and fresh `Box`, `Vec`, and
allocation probes link and run.

### G6: Build the bootstrap libraries and tooling surface

Add compiler-builtins, libc/unwind and panic crates, proc-macro transport and
execution, `std`, `proc_macro`, and `test`.  Complete deterministic host/target
DAG scheduling, build scripts/overrides, feature/cfg/environment handling,
native linking, and cache identity in C orchestration.

Proc macros and build scripts and their dependencies are host artifacts;
target libraries remain target artifacts and use separate output/cache
identities.  Build-script execution must provide Cargo's relevant environment
and consume normalized `cargo:rustc-*` cfg, link, flag, and environment output.
The current 1.90 recipe also relies on recorded overrides for
`compiler_builtins`, `libc`, and `std`.  cmrustc must advertise a distinct
`rust_compiler="cmrustc"` cfg, with the hashed source patch recognizing it
where the upstream recipe currently special-cases `mrustc`.

Acceptance: serial and parallel clean builds select the same pinned graph;
build-script and proc-macro round trips pass; hello and test binaries run.

### G7: Build `rustc` and `cargo` for `T`

Use the full patched `T` source graph and source-built native prerequisites.
The produced tools must not require cmrustc at runtime.

Acceptance: `rustc --version`, a hello program, a proc-macro crate, and a Cargo
workspace pass using only the produced toolchain.

### G8: Hand off to official Rust

Use the cmrustc-produced `T` rustc and Cargo to build `S`, then run the
upstream normalized stage comparison (binary equality where the pinned oracle
claims it).  From this point onward cmrustc is outside the release ladder.

Retain the upstream `run_rustc` staging semantics: rebuild the standard
library with the produced rustc, rebuild rustc against that library, and then
rebuild the final library so rustc, std, and proc macros share ABI and symbol
hashes.  During the transition, target compilations use the newer compiler
while host/build-script compilations use the previous one.  This is an ABI
bridge, not optional cleanup.

Acceptance: the `S` toolchain rebuilds itself and passes version, hello,
proc-macro, and workspace tests.

### G9: Climb to latest-at-run-time

Build each required official source release in sequence.  Record the source
and tool closure and require version, hello, and workspace gates at every rung.

Acceptance: the terminal compiler is the latest release selected by the
verified ladder policy, not a version hardcoded in cmrustc.

### G10: Close source-only provenance

Insert cmrustc and the C orchestrator into the pinned i386-musl live-bootstrap
chain, resolve the Rust host transition and source-built LLVM/CMake/native
dependencies, and repeat G3 through G9 without an unaccounted binary input.

Acceptance: a clean seed-to-terminal rebuild has a complete source closure and
reproduces all artifact and runtime gates above.

## Priority and risk

The first engineering priority is now G4: drive this consumed executable
artifact through the first real-`core` failure instead of widening a
declaration census.  G1 and G2 remain parallel obligations.

The largest risks, in order, are general body/MIR semantics; complete and
authenticated cross-crate executable artifacts; proc macros/build scripts and
native dependencies; and target ABI/runtime plus the source-only host
transition.  Orchestration is broad, but upstream minicargo supplies a useful
behavioral oracle and a known crate graph.

For every newly reached Rust construct: reduce the first real failure to a
focused positive and negative fixture, compare with upstream mrustc/rustc,
implement the bounded semantic slice, run strict and TinyCC gates, and then
rerun the deepest real artifact consumer.
