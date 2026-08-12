#![feature(no_core)]
#![no_core]
#![no_main]

fn add_pair(left: u32, right: u32) -> u32 {
    left + right
}

#[no_mangle]
pub extern "C" fn probe_chain(left: u32, right: u32) -> u32 {
    add_pair(add_pair(left + 1u32, right + 2u32), left + 3u32)
}

#[no_mangle]
pub extern "C" fn probe_after_call(left: u32, right: u32) -> u32 {
    add_pair(left + 1u32, right * 2u32) + (left + 3u32)
}
