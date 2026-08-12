#![feature(no_core)]
#![no_core]
#![no_main]

trait Missing {}

pub const fn bounded<T: Missing>(x: T) -> T {
    x
}

#[no_mangle]
pub extern "C" fn probe(x: u32) -> u32 {
    bounded::<u32>(x)
}
