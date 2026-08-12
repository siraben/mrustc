// Minimal support for the Rust 1.90 `Future` and `Ready<T>` declaration
// extracts in the sibling modules. The upstream documentation and method
// body are intentionally omitted; attributes and signatures are unchanged.
pub mod pin;
pub mod marker;
pub mod task;
pub mod future;
pub mod ready;
pub mod pending;
