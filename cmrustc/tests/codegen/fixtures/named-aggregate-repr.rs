#![feature(no_core)]
#![no_core]
#![no_main]

#[repr(C)]
struct Pair {
    first: u32,
    second: u32,
}

#[no_mangle]
pub extern "C" fn rejected_aggregate_repr(x: u32) -> u32 {
    Pair { first: x, second: x + 1u32 }.second
}
