#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn select(left: u32, right: u32) -> u32 {
    if left == right {
        left + 1u32
    } else {
        left - right
    }
}
