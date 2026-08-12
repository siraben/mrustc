#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn add(left: u32, right: u32) -> u32 {
    left + right
}
