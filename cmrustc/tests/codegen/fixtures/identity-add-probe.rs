#![feature(no_core)]
#![no_core]
#![no_main]

pub const fn identity<T>(x: T) -> T {
    x
}

#[no_mangle]
pub extern "C" fn probe_add(left: u32, right: u32) -> u32 {
    identity::<u32>(left + (1u32 + right))
}
