#![no_core]

mod x { pub struct Clash; }
mod y { pub struct Clash; }
use x::*;
use y::*;
