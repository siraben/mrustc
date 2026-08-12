#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn sub(end: u32, start: u32) -> u32 {
    end * start
}
