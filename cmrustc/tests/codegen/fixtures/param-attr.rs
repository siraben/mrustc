#![feature(no_core)]
#![no_core]
#![no_main]

// std's `unsafe fn reset_sigpipe(#[allow(unused_variables)] sigpipe: u8)`:
// outer attributes on function parameters.
fn scale(#[allow(unused_variables)] unused: u32, #[allow(unused_mut)] mut x: u32) -> u32 {
    x = x * 3;
    x
}

#[no_mangle]
pub extern "C" fn param_attr(x: u32) -> u32 {
    scale(0, x)
}
