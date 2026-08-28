#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn probe_closure(base: u32, value: u32) -> u32 {
    let add_base = |x: u32| x + base;
    add_base(add_base(value))
}

#[no_mangle]
pub extern "C" fn probe_direct(value: u32) -> u32 {
    let double = |x: u32| x * 2u32;
    double(value)
}
