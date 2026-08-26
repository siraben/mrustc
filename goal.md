# cmrustc overnight goal: build rustc end to end

Make the maximum safe, verified progress toward a fully functioning Rust
compiler written in C by making **cmrustc compile the same Rust release that
the retained upstream C++ mrustc demonstrably supports**. The current pinned
target is Rust 1.90.0. Do not redirect the effort toward parsing a newer Rust
release; after producing a working 1.90.0 `rustc`, use the normal Rust
bootstrap/release ladder to reach newer releases.

The terminal objective is not a declaration census, metadata file, `core`
artifact, or isolated code-generation demo. It is a cmrustc-produced Rust
1.90.0 `rustc` executable that runs, reports the expected version, compiles a
small Rust program, and produces a working executable. Continue working until
that objective is genuinely met or no safe in-scope path remains.

## Evidence lock

Before relying on a result, record and preserve:

- the exact upstream mrustc revision used as the behavioral oracle;
- the exact Rust source archive, SHA-256, patches, overrides, target triple,
  compiler flags, and native dependencies;
- the deepest reproducible cmrustc gate and its exact command;
- whether a result came from cmrustc, upstream C++ mrustc, or an official Rust
  compiler.

Never silently use upstream C++ mrustc in the cmrustc-produced artifact path.
It may be run only as an oracle for behavior, formats, diagnostics, and known
bootstrap inputs.

## Work order

Follow the dependency frontier rather than accumulating disconnected tests:

1. Preserve and validate the current working tree. Run the deepest current
   Rust 1.90.0 `core` probe and reduce its first real failure to a focused
   fixture.
2. Implement the smallest general compiler capability that removes that
   failure. Do not add source-location, item-name, crate-name, or fixture-name
   special cases. Add positive, negative, determinism, and atomic-failure
   coverage for each capability.
3. Re-run the full current-source `core` gate after every frontier fix. Repeat
   until `core` becomes a fresh-process, consumable object-bearing rlib—not
   merely a declaration-HIR census or nonempty metadata blob.
4. Build and execute representative consumers of `core`, then advance through
   `alloc`, compiler builtins, panic/unwind support, proc-macro support, `std`,
   and the remaining bootstrap libraries. Each crate must produce a loadable
   artifact and pass a runtime canary before moving on.
5. Implement or extend the C-side rustc-compatible driver/orchestrator as the
   real build graph demands: crate types, `--extern`, search paths, cfg and
   features, target/host separation, build scripts, proc macros, native links,
   deterministic caching, archive/object handling, C compilation, and final
   linking.
6. Build the pinned Rust 1.90.0 compiler and Cargo graph with cmrustc. Verify
   `rustc --version`, compile and run a hello-world program, and compile a
   small crate exercising generics, traits, and a proc macro.
7. Only after the cmrustc-produced Rust 1.90.0 compiler works, hand off to the
   official bootstrap machinery and advance release by release. This ladder is
   subsequent work and must not weaken the 1.90.0 completion gate.

When several independent tasks are available, fan out bounded audits, oracle
comparisons, focused test construction, and safety reviews to subagents. Keep
one owner per edited area, integrate only reviewed work, and always keep the
main dependency frontier moving.

## Verification requirements

For every coherent checkpoint:

- pass focused strict C99 GCC tests with warnings as errors;
- pass the corresponding TinyCC tests;
- run relevant Clang ASan/UBSan/LSan coverage;
- check deterministic twin outputs where artifacts are involved;
- test fresh-process consumption, corruption/truncation, wrong identity,
  wrong target/configuration, and atomic preservation of existing output;
- run the deepest applicable `core`/library/bootstrap probe;
- inspect `git diff`, run `git diff --check`, preserve unrelated user changes,
  and make a coherent local commit when green.

Do not delete existing build directories, rewrite Git history, push changes,
or use destructive cleanup. Do not weaken tests or acceptance criteria merely
to cross a gate.

## Honest status rules

The strongest completed evidence is always the deepest nonempty artifact
produced by cmrustc, consumed in a fresh process, and exercised through its
next dependent stage. Keep `cmrustc/docs/BOOTSTRAP_PARITY.md`,
`cmrustc/docs/ROADMAP_190.md`, and `cmrustc/TASKS.md` synchronized with that
evidence.

Do not declare this goal complete until all of the following are true:

- cmrustc builds a Rust 1.90.0 `rustc` without an upstream mrustc binary in
  the producing dependency closure;
- the produced `rustc --version` succeeds and identifies the expected build;
- that produced `rustc` compiles a Rust source file into an executable; and
- the resulting executable runs with the expected output.

If the terminal objective cannot be reached in one unattended run, leave the
repository at the deepest green, committed checkpoint; record the exact next
failure, reproducer, diagnosis, and next implementation step; and keep the
goal active. Mark it blocked only after the same external blocker has been
confirmed repeatedly and all other safe, relevant work is exhausted.
