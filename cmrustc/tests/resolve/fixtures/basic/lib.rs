#![no_core]

pub mod alpha;

mod inline {
    pub mod nested;
    use crate::alpha::Thing;
    const INNER: usize = 1;
}

use alpha::Thing;

struct Root;
fn root_fn() {}
