#![feature(no_core)]
#![no_core]
#![no_main]

pub trait Present {}

impl Present for u32 {}

pub fn bounded<T: Present>(x: T) -> T {
    x
}

#[no_mangle]
pub extern "C" fn object_probe(x: u32) -> u32 {
    x + 1
}
