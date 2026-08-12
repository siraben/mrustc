# Source and tool pins

These pins define the initial project target. Updating one requires a task and
must preserve the previous reproducibility gate.

## Compiler oracle

- Development base: upstream `mrustc` commit
  `d0fffe5b34fee392c56c44bb1028a9df8de05a1c` (2026-08-02).
- Proven Rust 1.90 reproduction tag:
  `df7034215681d6562afea0ecc3c10847f67d27d7`
  (`rust1.90_reproduced`).
- The project planning began from the nixpkgs-compatible older commit
  `7392eca5bd4958cb184fb47cefbd4c7e4f43547b`; the project branch was rebased
  onto the development base before implementation.

## Rust source ladder

Official source URLs have the form
`https://static.rust-lang.org/dist/rustc-VERSION-src.tar.xz`.

| Version | SHA-256 |
|---|---|
| 1.90.0 | `6bfeaddd90ffda2f063492b092bfed925c4b8c701579baf4b1316e021470daac` |
| 1.91.1 | `66401bb815e236cc6b2aacbbe23b61b286c1fe27a67902e7c0222cfe77b3dbab` |
| 1.92.0 | `ebee170bfe4c4dfc59521a101de651e5534f4dae889756a5c97ca9ea40d0c307` |
| 1.93.0 | `e30d898272c587a22f77679f03c5e8192b5645c7c9ccc3407ad1106761507cea` |
| 1.94.0 | `0b53ae34f5c0c3612cfe1de139f9167a018cd5737bc2205664fd69ba9b25f600` |
| 1.95.0 | `62b67230754da642a264ca0cb9fc08820c54e2ed7b3baba0289876d4cdb48c08` |
| 1.96.0 | `b99ce16cdf0ecfc761b585ac84d131b46733465a02f8ecd0ff2de9713c62ee09` |
| 1.97.1 | `0ed06fdaffd4722a7702e0b4eebfafc897ab8f513e8e1b247cdd7e5c6df6ded2` |

Rust 1.97.1 is the stable channel dated 2026-07-16, compiler commit
`8bab26f4f68e0e26f0bb7960be334d5b520ea452`.

As of 2026-08-10, only the checksum-exact 1.90.0 archive is present in the
local project cache. Its extracted tree contains selected libraries rather
than the compiler/bootstrap sources. The 1.91.1 through 1.97.1 hashes above
match the current official distribution checksum endpoints, but those archive
bytes are not locally cached or content-verified yet.

## Stage0 and TinyCC

- Reference live-bootstrap checkout:
  `dd8ac27bf959344b9bcf5e876bdd7716879bbc70` from
  `/home/siraben/nixos-full-source-bootstrap/live-bootstrap`.
- TinyCC 0.9.27 tarball SHA-256:
  `de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c`.
- The reference checkout is dirty and detached; integration must use a clean
  worktree at the pinned revision.
- The development amd64 HCC checkout is
  `c1684269b4b2414befa5f3f72eceeb8628123044`. Its
  `tinycc.m2.precisely.m2` output has NAR hash
  `sha256-5NSKzJ4Vsc6gfq/gmquJwaDIS4glPEyys9SfAigepY4=`; the installed TCC
  binary has SHA-256
  `0a523db52cc19827d2954d7b6470bd377eed62eea8dfb967fc3cb0f8ffe8c296`.
  A forced local rebuild passed its stage3/stage4 fixpoint, but used a
  substituted HCC dependency, so this does not replace the final clean-seed
  provenance run.
