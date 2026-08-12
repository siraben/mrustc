#![feature(no_core)]
#![no_core]
#![no_main]

pub const fn identity<T>(x: T) -> T {
    x
}

#[no_mangle]
pub extern "C" fn probe(x: u32) -> u32 {
    identity::<u64>(x)
}
