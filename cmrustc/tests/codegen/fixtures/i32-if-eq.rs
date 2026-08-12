#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn select(left: i32, right: i32) -> u32 {
    if left == right {
        1u32
    } else {
        0u32
    }
}
