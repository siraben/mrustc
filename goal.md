# cmrustc overnight goal

Advance cmrustc to bootstrap gate G3 in
`cmrustc/docs/BOOTSTRAP_PARITY.md` without stopping until the complete G3
acceptance test passes.

First preserve and validate the current working-tree changes, including the
`TryFrom` coherence fix and bootstrap-parity documentation. Work toward a
fresh-process executable cross-crate artifact containing one public generic
function, one trait, and one trait implementation.

## Completion criteria

Completion requires all of the following:

- An isolated producer emits deterministic, nonempty metadata plus required
  object/archive members.
- A fresh consumer process loads only that artifact, resolves and instantiates
  the generic function and trait impl, links an executable, and produces the
  expected runtime result.
- Both strict GCC and TinyCC builds pass and produce the same observable
  behavior.
- Relevant Clang ASan/UBSan tests pass.
- Twin isolated producers emit identical artifacts.
- Corrupt, incomplete, stale, wrong-source, and wrong-target artifacts reject
  atomically without publishing output.
- No fixture-name special cases, unconditional coherence exemptions, fake
  declarations, or hidden use of upstream C++ mrustc appear in the produced
  artifact path.

## Required context and evidence

Read `cmrustc/docs/BOOTSTRAP_PARITY.md`, `cmrustc/docs/METADATA_V3.md`,
`cmrustc/docs/ROADMAP_190.md`, `cmrustc/TASKS.md`, and the retained upstream
mrustc implementation before choosing the implementation slice. Use upstream
mrustc as a behavioral oracle for formats, flags, MIR, symbols, and runtime
results, but not as an input to the cmrustc-produced artifact.

Do not claim completion from an HIR census, metadata bytes alone, or a process
that emits no usable artifact. The authority is the deepest nonempty artifact
successfully consumed by a fresh process and exercised at runtime.

## Working method

Work in small verified checkpoints. Add focused positive and negative tests
for every semantic change. Make coherent local Git commits after green
checkpoints, but do not push, rewrite history, delete existing build
directories, or modify unrelated user changes. Keep
`cmrustc/docs/BOOTSTRAP_PARITY.md` and `cmrustc/TASKS.md` honest about the
deepest passing artifact.

Use subagents in parallel for bounded read-only investigation, oracle
comparison, test review, and independent safety review. Keep implementation
ownership coordinated to avoid conflicting edits.

If a G3 dependency is temporarily blocked, switch to another G3 dependency,
capture the upstream compiler invocation contract from G2, or advance the
current whole-core G1 frontier starting at `core/src/error.rs:937`. Only
declare the goal blocked when no safe in-scope progress remains after
exhausting those paths.
