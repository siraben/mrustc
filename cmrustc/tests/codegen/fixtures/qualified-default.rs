#![feature(no_core)]
#![no_core]
#![no_main]

trait Value {
    fn value(input: u32) -> u32 {
        input + 1u32
    }
}

impl Value for u32 {}
impl Value for usize {}

#[no_mangle]
pub extern "C" fn qualified_default_u32(input: u32) -> u32 {
    <u32 as Value>::value(input)
}

#[no_mangle]
pub extern "C" fn qualified_default_usize(input: u32) -> u32 {
    <usize as Value>::value(input)
}
