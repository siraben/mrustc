# Bootstrap proof

## Target trust chain

This diagram is the intended end state, not a statement that `hcargo`, a
compiler-built Rust library, or a cmrustc-built rustc exists today.  The target
release is selected by the evidence policy in `BOOTSTRAP_PARITY.md`; the
currently proven upstream mrustc instance is Rust 1.90.0.

The intended provenance is:

```text
stage0 seed
  -> M2/Mes compiler and tools
  -> TinyCC 0.9.26
  -> musl
  -> TinyCC 0.9.27 built by 0.9.26
  -> TinyCC 0.9.27 rebuilds itself
  -> TinyCC builds cmrustc and hcargo on i386-musl
  -> later source-built build tools, GCC/C++, CMake, Python, zlib, and LLVM
  -> cmrustc builds Rust T std, rustc, and cargo
  -> upstream rustc self-host stages reach the first official successor S
  -> successive official source releases reach latest-at-run-time
```

Native dependencies of rustc, including LLVM, must be supplied by packages
later in the same live-bootstrap closure or built from source inside it. The
current chain has GCC/C++, Python, and zlib but no CMake package, so CMake or an
alternative LLVM build path is a named prerequisite. Host binaries may
accelerate development but are not evidence for the final proof.

## Current stage0-produced TinyCC path

The actively maintained first integration target is
`/home/siraben/blynn-bootstrap`, whose faithful flake output is
`tinycc.m2.precisely.m2`. Its chain is stage0/M2 through Blynn/Precisely and
HCC into TinyCC; TinyCC stage 3 rebuilds byte-identically as stage 4 before
stage 3 is installed. At commit `c1684269`, a forced local rebuild completed
that fixpoint and reproduced output
`ngj284h9ag7fq8m39ramj667379hndr2-tinycc-boot-hcc-m2-precisely-m2-unstable-2025-12-03`.
The installed 459,844-byte TCC has SHA-256
`0a523db52cc19827d2954d7b6470bd377eed62eea8dfb967fc3cb0f8ffe8c296`.

The in-tree `make stage0-hcc` gate consumes that compiler and bundled Mes libc
directly. It builds `cmrustc`, runs source, lexer, parser, and code-emission
smokes, then uses the same TCC output for the complete no-core
compile/link/execute acceptance suite. At historical `cmrustc` commit
`4363c1ed`, two independent build directories produced the same 807,524-byte
`cmrustc`, SHA-256
`91a316ca42472a89afcb17e933a31fde9d2292bfba6519f9f87df1e27cb84a6f`.
That proves same-source reproducibility at the named commit; it is not the
artifact identity of current HEAD. A release/provenance record must repeat the
twin build and bind its size/hash to the exact source revision.
The exact invocation is:

```sh
make stage0-hcc \
  STAGE0_TCC_ROOT=/nix/store/ngj284h9ag7fq8m39ramj667379hndr2-tinycc-boot-hcc-m2-precisely-m2-unstable-2025-12-03
```

The bootstrap Mes headers provide exact-width integer types but omit the C99
integer constructor macros and several implemented POSIX declarations. They
also lack `mkstemp`. The compiler supplies bounded compatibility declarations
and constants and uses same-directory `open(O_CREAT|O_EXCL)` followed by
`rename` for collision-safe atomic output publication.

This remains an x86-64, TinyCC 0.9.28-era development proof. The forced TinyCC
rebuild used a substituted HCC dependency, and the external checkout contains
untracked build/document outputs. A final provenance claim still requires the
clean pinned seed-to-HCC closure and the i386 live-bootstrap path below.

At exact `cmrustc` commit `05f38c38`, two isolated `git archive` source
snapshots were built concurrently with that same pinned TinyCC output. Both
complete stage0 smoke suites passed, and both produced the same 1,070,684-byte
`cmrustc` binary with SHA-256
`4d6716ce234415acf2d6f282e294098768d4b2545aec5027a3a0deefc825f69c`.
This refreshes the deterministic current-source development record without
changing its x86-64 or substituted-HCC qualification.

It complements rather than replaces the i386/TinyCC 0.9.27 live-bootstrap
proof below.

## Existing live-bootstrap insertion point

The reference chain is:

```text
/home/siraben/nixos-full-source-bootstrap/live-bootstrap
```

The audited revision is
`dd8ac27bf959344b9bcf5e876bdd7716879bbc70`. The reference checkout is dirty,
so provenance runs must use a clean clone or worktree at that revision.

The manifest invokes `tcc-0.9.27` five times in total. The initial pass 1 at
approximately manifest line 38 is Mes-linked. Pass 2 appears at line 61. After
musl pass 1, pass 3 at line 64 uses
`steps/tcc-0.9.27/pass3.sh`: it first builds a musl-linked compiler with
TinyCC 0.9.26 and then rebuilds that compiler with itself. Musl pass 2 and
TinyCC pass 4 follow at lines 65 and 66. Pass 5 is much later, at approximately
line 111 after binutils and another musl rebuild.

A tiny self-host compile canary belongs immediately after pass 3. The practical
`cmrustc-stage0` recipe belongs after musl pass 3 at approximately manifest
line 68, following TinyCC pass 4 and `grep`: the fuller generated musl headers
and the early float fixes are then available while the compiler still has the
stage0 provenance needed by this proof.

An exploratory i386 probe using a newer Nix TinyCC in `-m32` mode and pristine
musl 1.1.24 headers failed at `va_start`; that setup is not representative of
the live-bootstrap environment. Each live-bootstrap musl pass applies its
`va_list.patch`, which selects the i386 TinyCC ABI through `__TINYC__` without
pretending that TinyCC is GCC. With the checksum-exact TinyCC 0.9.27 and musl
1.1.24 sources plus the live-bootstrap patches, all 41 production `cmrustc`
translation units compile as i386 objects. Therefore the recipe must not add
`-D__GNUC__`, and `cmrustc` needs no `va_list` workaround; native linking and
execution remain part of the full integration gate.

The recipe must:

1. use a pinned source archive and SHA-256;
2. force `CC=tcc` and `AR='tcc -ar'`;
3. build without Python, CMake, C++, zlib, or host-generated source files;
4. require the built `cmrustc --version` smoke;
5. compile and run a Rust `no_core` program through generated C with the same
   TinyCC/Mes libc;
6. install `cmrustc` into the live-bootstrap prefix, and install `hcargo` once
   that currently unimplemented component exists.

There is not yet a `cmrustc` live-bootstrap package, manifest entry, source
archive/checksum, or focused stop condition. The pinned `rootfs.py` accepts
`--internal-ci cmrustc` as a value, but the manifest has no matching package or
conditional `jump: break`. A bounded proof still needs both immediately after
musl pass 3.

The strongest currently implemented gate is `INTERNAL_CI=pass1`; it is not a
cmrustc-bounded gate. A fresh bwrap target can run it with:

```sh
nix develop --command ./rootfs.py \
  --bwrap --arch x86 --target /absolute/fresh/tcc-pass1-proof \
  --external-sources --cores 4 --internal-ci pass1
```

That gate necessarily traverses the self-hosted TinyCC stages but stops only
after the later Linux build, so it is much broader and slower than the missing
cmrustc-focused gate. The strongest provenance proof repeats the same pass1
gate from the builder-hex0 seed under QEMU:

```sh
nix develop --command make \
  BOOTSTRAP_PLATFORM=qemu \
  BOOTSTRAP_OFFLINE=1 \
  BOOTSTRAP_IMAGE=tcc-pass1-proof.qcow2 \
  BOOTSTRAP_IMAGE_SIZE=32G \
  QEMU_RAM=4096 \
  QEMU_CORES=4 \
  EXTRA_BOOTSTRAP_ARGS='--internal-ci pass1' \
  bootstrap
```

Both runs must use a clean checkout at the pinned revision. Exact
cmrustc-package commands and hashes can be recorded only after the recipe and
focused stop land; the live-bootstrap checkout is not modified from this
repository.

Before the insertion milestone can close, the recipe and focused stop must be
added and the complete TinyCC build/smoke suite must run on the resulting
native i386-musl compiler. The current x86-64 development-host results do not
prove i386 integer and aggregate ABI, 64-bit arithmetic and varargs, `setjmp`,
response files, large translation units, or generated-program execution. The
required native classification is defined by `tests/abi/run.sh` and remains
unrecorded.

At this early manifest point, the smallest executable recipe gate is to build
and install `cmrustc`, emit C for `tests/codegen/fixtures/no-core-exit.rs`, link
it with the same installed TinyCC/Mes libc, and require exit status 7 with
empty output. The full `tests/run.sh` is not an honest pass1 dependency because
`awk` and `cmp` are introduced later in the manifest. Wider tests can run at a
later integration checkpoint without weakening the early native canary.

The development tree now has the bounded proof shape required by recipe step
5. A TCC-built `cmrustc` emits nonempty portable C for both the literal no-core
canary and an exact reachable generic-call canary. TCC emits nonempty
executables; the first exits 7 and the second proves
`probe(x) == identity::<u32>(x) == x` for zero, 37, `0x89abcdef`, and
`UINT32_MAX`, all with empty output. The generated wrapper calls a real emitted
identity specialization. A third canary emits a real two-argument u32 add;
its GCC- and TCC-linked harness proves ordinary, boundary, and wrapping cases,
including `UINT32_MAX + 1 == 0`. A fourth canary proves recursive add lowering
through a declared MIR temporary and two statement-ordered C additions, again
including wrapping cases. A fifth composes the same nested arithmetic with a
real emitted identity call, proving that pre-call MIR statements and the final
computed operand survive through portable C. A sixth calls an ordinary private
two-parameter function with two independently computed arguments; its harness
proves `probe_pair(left, right) == left + right + 3` modulo 2^32 and generated C
contains two ordered temporary assignments followed by a real two-argument
call. A seventh composes call results: one export passes an inner call result to
an outer call, while another adds a later expression to an inner call result.
Its harness proves both exports compute `2 * left + right + 6` modulo 2^32,
including wrapping vectors. GCC- and TCC-built compilers emit the same 1,102
byte C artifact with SHA-256
`3121d75d53590e428025b810cc83c1e794017eac13ff2fb9fe57f8492d232e23`.
An eighth canary adds two top-level immutable `u32` lets. Its first initializer
computes `left + 1`; its second calls the private `add_pair` with that local and
`right + 2`; the tail then adds `left + 3`. The emitted MIR/C order is the two
initializer computations, the real call into the second user local, the later
tail computation, and the return assignment. Its harness proves
`probe_let(left, right) == 2 * left + right + 6` modulo 2^32. GCC- and
TCC-built compilers emit the same 689-byte C artifact with SHA-256
`944b2e203c93278c1f8d45b3f8632894d29807a1637e31590ff5b4fb0ae9299c`.
Unsupported nested subtrees, operators, type mismatches, roots, and calls
preserve an existing artifact or publish none, private unreachable unsupported
code is omitted, and input aliases cannot be overwritten. This is not
milestone 1 or the final no-core provenance proof: the same gates still have to
run after the stage0 chain builds `cmrustc` on the initial i386-musl host.

## Bootstrap milestones

1. **Compiler provenance:** stage0-built self-hosted TCC produces a `cmrustc`
   whose checksum and smoke behavior match the development build.
2. **No-core:** `cmrustc` emits C, TCC compiles it, and the program's exit and
   output match the oracle.
3. **Libraries:** core, compiler_builtins, alloc, proc_macro, std, and test build.
4. **Rustc root:** the patched Rust `T` compiler crate graph links and runs.
5. **Self-host:** upstream Rust rebuilds itself to the deterministic comparison
   used by `TestRustcBootstrap.sh`.
6. **Current stable:** the verified official self-host ladder reaches the
   latest release selected at run time and builds a hello program plus Cargo
   workspace without `cmrustc` remaining in the runtime closure.

Passing a process that emits no artifact is not a milestone. Every gate names
the expected files, verifies they are non-empty, executes them where possible,
and records their producers.
