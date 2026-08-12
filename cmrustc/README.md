# cmrustc

`cmrustc` is a bootstrap-only Rust compiler written in conservative C. Its
purpose is to replace the C++ build dependency of `mrustc`: a self-hosted
TinyCC must be able to build `cmrustc`, and `cmrustc` must then build a real
upstream Rust toolchain far enough for `rustc` to take over.

The existing C++ compiler in this repository is retained as the behavioral
oracle. This is not a line-for-line translation. The new compiler uses C data
structures and ownership rules designed for maintainability and for the C
dialect accepted by TinyCC 0.9.27.

## Target

- Development oracle: upstream `mrustc` at the repository's pinned revision.
- First direct source target: Rust 1.90.0, the newest version proven by
  upstream `mrustc`.
- First self-host handoff: the official Rust 1.91.1 sources/toolchain.
- Current project completion target: Rust 1.97.1, the stable release on
  2026-07-16.
- Initial compiler host: static i386 Linux/musl, matching live-bootstrap's
  first fully self-hosted TinyCC 0.9.27 stage.
- Secondary development platform: x86_64-unknown-linux-gnu.

The first full Rust target is selected by an ABI gate. The candidates are
`i686-unknown-linux-musl` for a native continuation and
`x86_64-unknown-linux-musl` after a source-built transition. Upstream's
reproducibility oracle remains `x86_64-unknown-linux-gnu`.

The version ladder is deliberate. `cmrustc` first proves the known 1.90
bootstrap root, then the produced upstream compiler advances through official
successive releases. Supporting Rust 1.97 syntax directly in `cmrustc` is not
required if the verified self-host ladder reaches it.

## Principles

1. Assume the patched bootstrap sources are valid Rust. Reject ambiguity;
   never silently invent values or calls.
2. Omit checks whose only observable effect is rejecting invalid input,
   including borrow checking, lint completeness, and rich error recovery.
3. Preserve runtime semantics needed by valid programs: layout, drop order,
   trait selection, monomorphization, proc macros, ABI, and side effects.
4. Prefer conservative behavior when it is unobservable: leak memory rather
   than free too early, retain extra copies, serialize uncompressed metadata,
   and perform less optimization.
5. Every approximation has a named compatibility rule and a regression test.
6. Every milestone builds with TinyCC, not just GCC or Clang.
7. Upstream `mrustc` is an oracle, not an architectural template.

## Layout

```text
cmrustc/
  docs/          architecture, compatibility, and bootstrap proof
  include/       public subsystem schemas and base APIs
  src/           conservative-C implementation
  tests/         unit, differential, ABI, and bootstrap probes
  tools/         deterministic test and bootstrap helpers
  TASKS.md       durable task ledger and milestone gates
```

See [TASKS.md](TASKS.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md), and
[docs/BOOTSTRAP.md](docs/BOOTSTRAP.md). Exact source revisions and hashes are
in [docs/PINS.md](docs/PINS.md).
