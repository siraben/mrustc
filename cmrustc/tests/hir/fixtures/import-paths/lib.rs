extern crate self as local_core;

mod defs;
mod consumer;

pub use crate::defs::Public as Facade;
pub use crate::Facade as Surface;
