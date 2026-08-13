#![feature(no_core)]
#![no_core]
#![no_main]

fn add_pair(left: u32, right: u32) -> u32 {
    left + right
}

fn private_valid(value: u32) -> u32 {
    let ignored: u32 = value + 1u32;
    ignored
}

#[no_mangle]
pub extern "C" fn probe_let(left: u32, right: u32) -> u32 {
    let first = left + 1;
    let combined: u32 = add_pair(first, right + 2);
    combined + (left + 3)
}
