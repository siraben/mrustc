#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn helper() -> i32 {
    11i32
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    7i32
}
