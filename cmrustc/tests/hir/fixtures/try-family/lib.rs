// Minimal surrounding crate for exact Rust 1.90 `Try` declaration and
// compatible impl extracts. Bodies and documentation are intentionally
// minimized; declaration attributes and signatures remain upstream-shaped.
pub mod convert;
pub mod control_flow;
pub mod ops;
pub mod never_short_circuit;
