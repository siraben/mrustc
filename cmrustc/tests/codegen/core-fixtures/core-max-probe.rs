#![no_std]
#![no_main]

use core::cmp::max;

fn pick(a: u32, b: u32) -> u32 {
    max(a, b)
}

#[no_mangle]
pub extern "C" fn probe_core_max(a: u32, b: u32) -> u32 {
    pick(a, b)
}

#[no_mangle]
pub extern "C" fn probe_core_option(value: u32) -> u32 {
    let wrapped: Option<u32> = Some(value);
    match wrapped {
        Some(v) => v + 1u32,
        None => 0u32,
    }
}
