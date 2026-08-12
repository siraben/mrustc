#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn probe_let(left: u32, right: u32) -> u32 {
    let first: u32 = left * 1u32;
    first + right
}
