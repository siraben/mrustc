#![feature(no_core)]
#![no_core]
#![no_main]

fn add_pair(left: u32, right: u32) -> u32 {
    left + right
}

#[no_mangle]
pub extern "C" fn probe_chain(left: u32, right: u32) -> u32 {
    add_pair(add_pair(left + 1, right + 2), left + 3)
}

#[no_mangle]
pub extern "C" fn probe_after_call(left: u32, right: u32) -> u32 {
    add_pair(left + 1, right + 2) + (left + 3)
}
