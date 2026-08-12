#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn nested_add(left: u32, right: u32) -> u32 {
    left + (1u32 + right)
}
