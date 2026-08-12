#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn mixed(value: usize, other: u32) -> usize {
    value
}
