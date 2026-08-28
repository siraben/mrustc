# cmrustc → rustc 1.90.0 bootstrap plan

Goal: **cmrustc compiles the Rust 1.90.0 sources well enough to build
rustc 1.90.0, and the resulting rustc reaches a self-compile fixpoint**
(the cmrustc-built rustc rebuilds rustc+std, and that second compiler is
functionally equivalent — the mrustc bootstrap route, one version newer).

This document is the working map from A (today: core mostly typechecks)
to B (fixpoint), including which shortcuts are allowed, and how each one
is repaired later when we want rigor. Measured history lives in
`TASKS.md`; this file is direction, doctrine, and the task graph.

---

## 1. Where we are (2026-08-27, branch `siraben/cmrustc`)

Pipeline stages and status, in execution order:

| Stage | Status | Evidence |
|---|---|---|
| Lexer / parser / AST | solid | full parser test suite in lanes; binary precedence fixed to match Rust (`fcc31e3a`) |
| Module graph, cfg, item-macro expansion, `include!` | solid | whole core: `graph errors=0`, 451 modules |
| Import resolution | solid | glob, enum-variant globs, re-exports, macro-generated items |
| Body macro expansion (M9-01) | **done** | 14,889 invocations, 0 failures; only 56 `asm!` + 3 `offset_of!` retained |
| cfg-inactive *statement* stripping in bodies | done | `dcb484f5` |
| Untyped bodies `ubody` (M9-02) | **done** | 22,524/22,524 bodies, 321,714 exprs, **33** unresolved paths |
| Inference typeck (M9-04) | two-crate residue mode | core-only run 64: **151** error nodes; two-crate (core+alloc, census26): 23,071/24,797 typed, **420** error nodes over 352,590 exprs |
| Whole-context snapshot + multi-crate (M9-03) | **HIR/ubody acceptance met** (census26, `16e5eab2`) | alloc lowers against in-memory core with 0 errors (42,158 items); ubody 24,797/24,797; dependency imports/prelude/macros/coherence leniencies landed |
| MIR (M9-05) | **ubody->u-MIR expression coverage complete** (census81, `ab7a3023`) | all 23,326 typed bodies emit complete u-MIR (260,137 statements, 46,280 blocks, empty opaque histogram); const eval (S7) and the 1,616 partial-tyck bodies remain |
| C codegen + link (M9-06) | **first executable gate green** (`442ee29f`) | u-MIR C emitter: full-tree sample 101,075 bytes compiles; `run_umir_echo.sh` builds and runs the no_core echo fixture against its C harness |

Key numbers trend (whole-core `--body-census`): typed 16,339 → 17,101 →
18,196 → 19,928 → 20,323 → 20,437 → 20,663 → 20,854; error nodes 12,858
→ … → 747. Remaining error classes (run 14): method-arg 160 (≈95 is one
`ByteStr` family), call-arg 127, method-not-found 111 (Simd/Pattern
predicate subjects — fix in flight), body-vs-signature 103, non-fn
callee 70 (f64/`()` callees, kind still unidentified), branch 33,
unresolved value path 33 (`impl_Display!` `fmt_u64` helpers), field 19,
assoc-value 16.

Uncommitted right now: `src/hir/tyck.c` (structured-predicate gating,
SliceIndex trait-arg matching in normalize, non-fn debug) — commits after
run 16 lands with numbers.

### Infrastructure that exists and must stay green

- **Measurement**: `./build/probe-ref6/probe_core_hir <core>/src/lib.rs
  --body-census` (~13 min) → `ubody`/`tyck` lines + error histograms.
  `--source-map` dumps source-id → path (~1 min). Record numbers in
  `TASKS.md` per pass.
- **Debug**: `CM_TYCK_DEBUG=1 CM_TYCK_DEBUG_LIMIT=N` (typed pairs +
  spans per error class), `CM_UBODY_DEBUG=1` (unresolved paths, binding
  joins), `CM_BODY_EXPAND_DEBUG=1`.
- **Lanes** (all green before every commit):
  `nix-shell -p gcc tinycc --run 'make -j8 test'`,
  `nix-shell -p clang tinycc --run 'make -j8 strict STRICT_CC=clang'`,
  `nix-shell -p tinycc --run 'make -j8 tcc'`.
- **ASan/UBSan** one-liner over `tests/hir/test_tyck.c` + all sources.
- Minimal repro corpus in scratchpad (`simdrepro/*.rs`) — promote the
  useful ones into `tests/` fixtures as they stabilize.
- Rust 1.90.0 sources: `/tmp/cmrustc-rust190-source/rustc-1.90.0-src/`
  (re-fetchable; nothing else lives in /tmp that matters).

### Hard-won invariants (do not regress)

- **Never hold a `const CmTy*` across any call that can create types**
  (`cm_ty_resolve` can). Copy counts/children to locals first; children
  arrays are type-owned and stable, the `CmTy` structs are not.
- **Binding→HIR joins are by AST identity** (`CmHirItem.ast_source` /
  `.ast_item`), never by span — generated items have coarse anchor spans.
- Interner: push-then-intern; each type owns its children array.
- `cm_lower_item_header` memsets `out_item` — set provenance *after* it.

---

## 2. Doctrine: allowed shortcuts and the repair contract

We are building the **smallest compiler core that can bootstrap**, in
mrustc's spirit: input is assumed to be valid Rust 1.90.0. We do not
diagnose; we only need to *agree with rustc on what the code means*.

Allowed permanently (mrustc precedent — never needs repair for the goal):

- No borrow checking, no lifetime solving (lifetimes are positional
  placeholders).
- No coherence/overlap checking, no stability/feature-gate checking.
- No diagnostics quality; first error can be terse.
- Accept-only leniency in *typeck* where the meaning is still recovered.

Allowed **temporarily**, with a repair path (the register in §3):
anything that guesses meaning rather than proving it — because a wrong
guess produces wrong *code*, not just a missing error. The contract for
every such shortcut:

1. It must be **locally identifiable**: mark the site with a comment
   containing `LENIENT` (existing sites use prose comments; when
   touching one, upgrade it to a `/* LENIENT(tag): ... */` marker).
2. It must be **countable**: prefer wiring a counter/histogram (like the
   tyck error classes) over silent acceptance, so we can measure how
   often a guess actually fires when compiling the real tree.
3. Its **repair** must be written down here (§3) at introduction time.
4. Leniency may decide *types* during M9-04, but by M9-05 (MIR) every
   place codegen consumes a type must either be concrete or the leniency
   must be shown unreachable on the bootstrap tree (counter = 0).

Scalability rule (how we go A→B repeatedly): the loop that got core from
12,858 → 747 error nodes is the template for every later stage —
**instrument classes → sample with debug env → fix the biggest family →
whole-tree measure → lanes → commit with numbers in TASKS.md**. Every new
stage (MIR, codegen, each new crate) gets the same shape: a whole-tree
probe with per-class histograms, and env-gated sampling.

---

## 3. Shortcut register (current)

| # | Shortcut | Where | Risk | Repair when needed |
|---|---|---|---|---|
| S1 | Deep lenient structural equality: any position holding PROJECTION / PARAM / SELF / OPAQUE / const arg is accepted in coercion | `cm_tyck_lenient_eq` | wrong types flow into MIR | real obligation solving: prove `T: Trait` from predicates before accepting; make `lenient_eq` count hits per kind |
| S2 | Projection normalization picks first matching impl (blanket-demoted, trait-args matched) without checking where-clauses | `cm_tyck_normalize` | picks wrong impl → wrong assoc type | check impl predicates against known facts; on ambiguity defer instead of committing |
| S3 | Method lookup: two-phase (inherent-all-derefs, then trait), no predicate proof on the chosen impl, no real autoref | `cm_tyck_method_call`, `cm_tyck_lookup_assoc_in` | wrong method chosen (the `Hasher::finish` class) | rustc-style probe: collect all candidates, filter by predicate satisfaction, pick by rustc's order |
| S4 | Operators on PROJECTION/PARAM/SELF/const operands return the operand type | `cm_tyck_binary` | `Output ≠ Self` cases mistyped | resolve the operator trait impl and take its `Output` |
| S5 | Deref of a bare primitive returns the operand (masks a `&`-pattern binding bug) | unary DEREF | **masks a real pattern-typing bug** | fix REF-pattern vs scrutinee stripping in `cm_tyck_pat`, then delete this arm and confirm counter stays 0 |
| S6 | Structured predicate subjects match receivers via `cm_tyck_matches` (PARAM wildcards), bare-PARAM subjects exact-only | lookup_assoc predicate scan | overmatch if two bounds differ only in params | instantiate the predicate subject and *unify*, not wildcard-match |
| S7 | Const generics: `CONST_UNKNOWN` unifies with anything; no const evaluation | `ty.c` | array lengths/discriminants unknown | **must repair for M9-05**: const evaluator (see task list) |
| S8 | Body-local `use` via textual use-tree parsing; globs tried last | `ubody.c` | precedence subtleties | acceptable; if a miss appears, resolve through the real import resolver machinery |
| S9 | cfg stripping covers statements/tails only (not match arms, fields, fn params inside bodies) | `body_expand.c` | inactive code retained → spurious unresolved | extend the same filter to the remaining attribute positions when a real case appears |
| S10 | 59 `asm!` + 3 `offset_of!` retained as opaque text | body expansion | cannot codegen those bodies | M9-06: implement `offset_of!` via layout; `asm!` → keep text through MIR into the C backend as gcc inline asm (tcc lane: link those objects with gcc, as mrustc does) |
| S11 | 743 body-local `fn`/`struct` items are ubody-resolved but not lowered as real HIR items | `ubody` NESTED_ITEM | no MIR for nested fns | M9-05: lower nested items as HIR items with mangled parents |
| S12 | `unresolved value path` 33 residue (`impl_Display!` `fmt_u64` family, avx2 helpers) | ubody | those bodies mistype | diagnose with `CM_UBODY_DEBUG` when they matter; likely generated-item namespace gap |
| S13 | Trait-fn DEFINITIONs get fresh-Self FN_DEF slot; `Self` encoding is positional convention | fn_def/call | fragile if arg layout changes | fold Self into an explicit FN_DEF field when MIR needs it |
| S14 | No drop elaboration plan yet; will start with mrustc-like simplified scope drops | future M9-05 | leaks/双 frees if wrong | follow mrustc's drop rules; validate with std tests at M10 |

---

## 4. Milestones A → B

Each milestone lists: goal, tasks, exit criterion, and its measurement.
Order is chosen so that every stage has a whole-tree metric from day one.

### M9-04 — finish core typeck (ACTIVE)

Goal: every core body typed, or residue individually explained.

- [ ] Land the in-flight pass (structured predicate subjects, SliceIndex
      trait-arg matching) once run 16 confirms no regression.
- [ ] `ByteStr` method-arg family (~95): identify why `&[u8]`/`&str`
      receivers select `ByteStr` methods; likely macro-generated
      `impl PartialEq<...>` pairs — fix selection, not coercion.
- [ ] Non-fn callee f64/`()` (~70): samples now print callee expr kind —
      identify and fix (suspect: qualified-path or cast callees).
- [ ] Remaining body-vs-signature (~100): sample-driven, same loop.
- [ ] Drive the S5 deref counter to zero by fixing REF-pattern typing.
- [ ] Exit: **error nodes < ~100 with every remaining class explained**,
      then freeze: promote key repros into `tests/hir/test_tyck.c`.

### M9-04V — leniency accounting (small, do before MIR)

- [ ] Add counters for every `LENIENT` site (like tyck error classes);
      print under `--body-census` as `tyck-lenient class=N`.
- [ ] Whole-core run: the histogram is the priority list for repairs
      that matter (many will be 0 — those are free).

### M9-03 — whole-context snapshot + multi-crate

Goal: `alloc` and `std` (and the small std deps) lower + typecheck
against a loaded `core`.

- [ ] Crate snapshot: serialize/hold HIR + impls + namespaces of a
      compiled crate in memory first (single process compiles the whole
      DAG — simplest and fine for bootstrap); on-disk metadata later
      only if memory forces it.
- [ ] Extern-crate wiring in resolver/ubody/tyck (paths beginning with a
      dependency crate name; `no_std` prelude rules).
- [ ] Compile order for std stack: `core` → `compiler_builtins` (Rust
      parts) → `alloc` → `cfg_if`/`libc` (mostly decl-only) →
      `hashbrown` → `std_detect` → `panic_unwind`/`panic_abort` →
      `unwind` → `std`.
- [ ] Probe generalization: `probe_core_hir` grows `--crate <name>`+
      dependency flags (or a small driver manifest) so *every* crate has
      the same census loop.
- [ ] Exit: std stack fully ubody-lowered and typed with the same
      residual standard as core.

### M9-05 — MIR for all bodies (+ const eval)

Goal: typed ubody → MIR for every body in the std stack.

- [ ] MIR model: locals, statements (assign, ref, aggregate, cast,
      discriminant), terminators (call, switch, return, unwind-less
      panic path first), scopes with simplified drops (S14).
- [ ] Lowering from ubody using tyck's `expr_types`/`pat_types` tables
      (already persisted per body).
- [ ] **Const evaluator** (repairs S7): interpret MIR at compile time —
      needed for array lengths, enum discriminants, `const fn`, statics,
      `offset_of!`. Start with the integer/bool/char/ptr subset core
      actually uses; measured by a `const-eval` census class.
- [ ] Nested body items (repairs S11).
- [ ] Census: `mir bodies=X lowered=Y failed-class histogram`, same loop.
- [ ] Exit: 100% of std-stack bodies produce MIR; const-eval failures
      have a histogram with explained residue.

### M9-06 — C emission + link

Goal: MIR → C99 → objects → executables, gcc and tcc lanes.

- [ ] Layout engine (sizes/alignments/niches; repr(C), repr(transparent),
      repr(simd) minimal), monomorphization collector (start from `main`
      / used items, walk MIR).
- [ ] Name mangling stable across crates; vtables for `dyn`.
- [ ] Intrinsics table: implement the ones the std stack actually uses
      (census first, implement by count); `asm!` passthrough (S10).
- [ ] E2E gates, in order: (1) `#![no_core]` hello-exit; (2) core-only
      binary using fmt; (3) std hello world; (4) run a std unit-test
      binary subset.
- [ ] Exit: std hello world runs; e2e fixtures in the lanes.

### M10 — driver + build orchestration

Goal: build whole crate DAGs unattended.

- [ ] CLI compatibility with what mrustc's **minicargo** expects
      (`--crate-name`, `--crate-type`, `--extern name=path`, `--cfg`,
      `-L`, `-o`, edition flags) — then reuse minicargo unchanged to
      drive cmrustc over vendored crates. This is the cheapest scalable
      path; a native mini-driver only if minicargo compat turns out
      harder than expected.
- [ ] Exit: `minicargo` (pointed at cmrustc) rebuilds the std stack
      from `library/` unattended.

### M11 — proc macros

Goal: rustc's own crates use `rustc_macros` (derive/attribute proc
macros) and some vendored proc-macro deps.

- [ ] mrustc's approach: compile the proc-macro crate to a native
      executable linked against our std, speaking mrustc's token-stream
      protocol over stdio; expansion invokes the exe per call site.
      Adopt the same protocol so mrustc's design (and test corpus) maps
      over.
- [ ] Standard derives (`Clone`, `Debug`, `PartialEq`, …) are already
      builtin-expanded at AST level — keep that path; proc-macro exe is
      only for real proc-macro crates.
- [ ] Exit: a fixture derive crate round-trips; `rustc_macros` expands.

### M12 — compile rustc 1.90.0

- [ ] Vendor scan: census the `compiler/` crate DAG + vendored deps;
      run the ubody/tyck/MIR loops over the full set (this is where the
      per-class histograms scale: same loop, bigger tree).
- [ ] Expect new language surface: more const generics, GATs-lite uses,
      `dyn` upcasting, closures in const… — burn down by class.
- [ ] Link a working `rustc` binary (LLVM: rustc 1.90 needs LLVM —
      link against a system/prebuilt LLVM the same way mrustc's
      bootstrap does; codegen backend crates are C++-linked, driven by
      build scripts we replicate by hand like mrustc's `rustc-build`
      scripts).
- [ ] Exit: cmrustc-built `rustc --version` runs and can compile hello
      world with `--sysroot` of cmrustc-built std.

### M13 — fixpoint

- [ ] Stage1 (cmrustc-built rustc) builds std + rustc → stage2.
- [ ] Stage2 rebuilds std + rustc → stage3; require stage2 ≡ stage3
      (bit-identical with deterministic flags, or at minimum: identical
      test-suite behavior + self-rebuild stability).
- [ ] Exit = **the goal**: record the exact commands in `TASKS.md`; add
      a scripted `make bootstrap-fixpoint` target.

---

## 5. Risks / unknowns (watch list)

1. **Const eval breadth** — core/std lean on `const fn` heavily; this is
   the biggest unbuilt subsystem (M9-05). Mitigation: census-first,
   implement by frequency.
2. **Proc macros** (M11) — second biggest. Mitigation: mrustc's protocol
   is proven; copy it.
3. Trait-solving debt (S1–S6) surfacing as *miscompiles* rather than
   type errors — mitigation: M9-04V counters + spot-check MIR types on
   sampled bodies against `rustc -Zunpretty=mir` for a fixture corpus.
4. LLVM linkage for rustc itself — pure build-engineering; mrustc shows
   it works at 1.29/1.39-era; check what 1.90 bootstrap needs (its
   `bootstrap` does much more; we bypass it like mrustc does with
   hand-written build scripts).
5. Memory: whole-DAG in one process (M9-03 choice) — measure; core alone
   is fine, rustc DAG may need per-crate metadata after all.

---

## 6. Operational context dump

- Worktree: `/home/siraben/mrustc/cmrustc` (branch `siraben/cmrustc`;
  never bare `git stash` — shared stack).
- Probe binary: `build/probe-ref6/probe_core_hir` (`make
  BUILD_DIR=build/probe-ref6 build/probe-ref6/probe_core_hir`).
- Core sources: `/tmp/cmrustc-rust190-source/rustc-1.90.0-src/library/`.
- Latest whole-core outputs: `/tmp/cmrustc-core-probe-tyck*.out`
  (run 14 = committed numbers; 15–16 = predicate-search iteration);
  source-id map: `/tmp/cmrustc-core-source-map.txt`.
- Key files: `src/hir/{ty,tyck,ubody,lower}.c`, `src/resolve/
  {body_expand,imports,module_graph}.c`, `src/macro/*`,
  `tools/probe_core_hir.c`; tests in `tests/hir/`, `tests/resolve/`.
- History and per-pass numbers: `TASKS.md` (M9 section);
  `docs/ROADMAP_190.md` "Revised strategy"; compat notes in
  `docs/COMPATIBILITY.md`.
- Session memory (cross-conversation): `cmrustc-e2e-mission` +
  `cmrustc-probe-workflow` memory files mirror this plan's state.
