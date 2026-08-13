#![feature(no_core)]
#![no_core]
#![no_main]

fn first(x: u32) -> u32 {
    second(x)
}

fn second(x: u32) -> u32 {
    first(x)
}

#[no_mangle]
pub extern "C" fn probe_mutual(x: u32) -> u32 {
    first(x)
}
