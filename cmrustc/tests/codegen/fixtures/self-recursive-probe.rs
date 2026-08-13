#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn probe_recursive(x: u32) -> u32 {
    probe_recursive(x)
}
