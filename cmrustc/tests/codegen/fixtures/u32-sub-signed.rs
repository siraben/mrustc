#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn sub(end: i32, start: i32) -> i32 {
    end - start
}
