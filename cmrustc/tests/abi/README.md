# C producer capability probes

`run.sh` separates properties needed by the compiler from producer extensions
that `cmrustc` can avoid or emulate. A required probe fails the command. An
extension probe records either `pass` or the planned fallback:

| Probe | Required behavior | Fallback when absent |
|---|---|---|
| `core` | integer widths, alignment, aggregate ABI, varargs, 64-bit arithmetic, `setjmp` | none |
| `tls` | producer `__thread` extension | avoid TLS in the compiler executable |
| `atomics` | producer `__sync_*` extension | lock-based runtime implementation |
| `weak` | weak definition override | emit deterministic strong symbols |
| `sections` | named function/data sections | omit section splitting |
| response file | driver response-file handling | none for the Rust-scale link |
| large translation unit | at least 4,096 chained functions | none |

Run it with the exact producer being qualified:

```sh
CC=tcc CFLAGS='-O2 -std=c99 -Wall -Werror' tests/abi/run.sh
```

The native i386-musl result must be captured inside live-bootstrap after TCC
self-hosts. Development-host results do not close M1-01 or M1-07 by themselves.

## Development-host baseline

On x86-64 GNU/Linux, GCC 14.3 passes every probe. TinyCC 0.9.27 passes the
required core, response-file, large-translation-unit, weak-symbol, and section
probes. Its TLS and atomic probes select the documented `avoided` and
`emulated-lock` paths. This baseline is diagnostic only; the authoritative
i386-musl classification remains pending.
