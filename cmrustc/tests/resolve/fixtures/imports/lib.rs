#![no_core]

pub mod a {
    pub struct Thing;
    pub struct Pair(pub usize);
    pub fn value() {}
    pub struct Public;

    pub mod nested {
        pub struct Deep;
    }
}

pub mod b {
    pub const FLAG: bool = true;
    pub use crate::a::Thing as ReThing;
}

use crate::a::{Thing as Alias, value, Pair, nested::{Deep}};
pub use crate::a::Public;
use self::b::*;
use ::a::Thing as AbsoluteThing;

mod inner {
    pub use super::a::Thing as SuperThing;
}
