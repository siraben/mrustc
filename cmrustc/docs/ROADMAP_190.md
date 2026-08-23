# Rust 1.90 bootstrap roadmap

This is the development critical path from the audited 2026-08-23 tree to a
bootstrapped upstream Rust 1.90.0 compiler. `TASKS.md` remains the detailed
ledger; this document orders its tasks by the deepest consumable artifact.

## Audited baseline

- The audit began at `a6e7c309`. Commits through `1630867d` added declaration
  const terms, generic public functions, lossless public-function predicate
  capture, and canonical callable lifetime elision.
- The C implementation has 129,227 production C/header lines and 101,804
  C/header/shell test and tool lines. It is a substantial fork, but the normal
  rustc-shaped CLI still reports that the compiler pipeline is unimplemented.
- `make check-core-hir` is the strongest completed Rust-library gate. At
  `6b63926f`, target-configured Rust 1.90 `core` loaded 363 sources and 451
  modules with zero graph, import, or HIR errors, producing 38,176 items,
  22,524 bodies, and 159,528 types.
- The next measured boundary contains 252 traits, 9,092 impls, and 9,854
  generic parameters, including 2,411 const and 1,443 lifetime parameters.
  Commits `fdfbe33c` through `a6e7c309` began extending library capture and
  declaration metadata for that surface.
- This audit extends declaration metadata through v2.4. Version 2.3 transports authenticated
  literal and parameter const uses in named generic arguments and array
  lengths, plus predicate-free public free functions with lifetime, type, and
  const parameters. Version 2.4 adds a bounded public-function predicate
  section and opaque reference-only trait/associated identities. Strict v2.4
  decode falls back to exact legacy v2.3 only on an unsupported-version result;
  legacy v1.0/v1.1 bytes remain unchanged.
- No current `.rlib`, compiler-built `core`, `alloc`, `std`, `rustc`, or
  `cargo` artifact exists. M6-06 is therefore the active vertical milestone.
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
  zero bytes. With v2.4 the next real-core result must be re-measured; direct
  trait-alias predicates such as `Thin` remain deliberately unsupported until
  alias-bound provenance has a sound wire representation.
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

### P1: Consumable `core` declaration artifact (M6-06)

1. Make `check-core-metadata` capture the value-aware library artifact and
   encode the v2 declaration format without omitting public declaration
   families. The gate must fail on unsupported HIR and require nonempty bytes.
2. Extend library capture and the versioned wire format in bounded slices:
   public-value predicates, traits and trait aliases, associated declarations
   and projections, impl headers, and dependency-backed type edges. Preserve
   deterministic local handles, atomic decode rollback, and fail-closed
   version checks.
3. Decode the complete artifact in a fresh HIR context, re-encode it
   byte-identically, and compile a dependent fixture that uses a generic type,
   public value, trait, associated type, and impl from the decoded crate.
4. Put the checked metadata in the deterministic SysV archive container and
   teach the driver to load that container. Call it an `.rlib` only when the
   archive contract and any required object members are explicit.

Acceptance: two isolated producers emit identical nonempty core artifacts; a
fresh process loads one, resolves the dependent fixture without producer
state, and rejects corrupt or semantically incomplete artifacts atomically.

### P2: One executable cross-crate core slice (M6-08 before breadth)

Carry one generic public function plus one trait implementation across the
artifact boundary through body typing, trait selection, MIR, instance
reachability, layout, C emission, object production, archive loading, linking,
and execution. This vertical canary comes before broadening declaration-only
coverage that no later stage consumes.

Acceptance: GCC and TinyCC development builds produce the same observable
result; unsupported substitutions, missing impl facts, and stale artifacts
hard-fail without preserving a newly written output.

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
