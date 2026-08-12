# Lexer differential evidence

`run_differential.sh` compares cmrustc's lexer with the lexer in the pinned,
already-built upstream mrustc checkout.  It does not modify or rebuild
`bin/mrustc`.

The upstream command-line interface can stop after parsing, but it cannot dump
the pre-parser token stream.  `cmrustc/tools/mrustc_lexer_oracle.cpp` is
therefore a developer-only adapter linked with the existing `bin/mrustc.a` and
`bin/common_lib.a`.  It calls the same `Lexer::getTokenInt()` implementation as
the executable.  A separate gate invokes the actual executable with:

```text
MRUSTC_TARGET_VER=1.90 bin/mrustc --edition 2021 --crate-type rlib \
    -Z stop-after=parse oracle_accept_2021.rs
```

The script hashes `bin/mrustc` before that command and verifies the hash after
it.  The adapter is not part of cmrustc's C/TCC build or its trust closure.

## Canonical comparison

The comparison deliberately retains only information represented reliably by
both lexer APIs:

- token order and token category;
- exact identifier, raw-identifier, lifetime, keyword, and punctuation text;
- whether a literal is integer/character, float, string, byte string, or C
  string;
- keyword behavior for editions 2015, 2018, and 2024, except for the documented
  `gen` difference.

Whitespace and comments are omitted. Raw and cooked forms with the same value
category are normalized together. Character and byte-character literals are
normalized as integers because upstream represents both with `TOK_INTEGER`.
Literal values, escape decoding, raw delimiter counts, numeric bases/suffix
spans, byte offsets, and source positions are not compared by this adapter;
the native C lexer unit tests cover its retained spelling metadata.

The exact fixtures cover nested ordinary comments, raw identifiers, lifetimes
versus character literals, known numeric suffixes, punctuation gluing, cooked
and raw strings, and the 2015/2018/2024 keyword sets accepted by upstream.

## Explicit known differences

The harness requires these streams to remain different, so they cannot be
mistaken for untested agreement:

1. `cr#"..."#`: cmrustc recognizes modern raw C strings as one token. Upstream
   splits them into an identifier and punctuation/string tokens.
2. `gen` in edition 2024: cmrustc marks the Rust 2024 reserved keyword.
   Upstream's lexer still uses its 2018 keyword table for edition 2024.
3. An unknown numeric suffix: cmrustc retains the suffix in the literal token,
   matching Rust's lexical model. Upstream emits the unknown suffix as a
   following identifier.

Doc comments are also not part of exact token comparison: upstream converts a
doc comment into synthetic `#[doc = ...]` tokens inside its lexer, whereas
cmrustc retains a comment token and style flags for a later lowering pass.

## Running

From the repository root, in an environment containing a C99 compiler, a C++14
compiler, zlib, and pthreads:

```text
cmrustc/tests/syntax/run_differential.sh
```

Compiler and library paths can be overridden with `CC`, `CXX`, `MRUSTC`,
`ORACLE_ARCHIVE`, `ORACLE_COMMON`, and `ORACLE_LDLIBS`.
