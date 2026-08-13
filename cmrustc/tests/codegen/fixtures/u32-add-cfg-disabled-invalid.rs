#![feature(no_core)]
#![no_core]
#![no_main]

#[cfg(any())]
fn dormant(left: u32, right: u32) -> u32 {
    left * right
}

#[no_mangle]
pub extern "C" fn add(left: u32, right: u32) -> u32 {
    left + right
}
