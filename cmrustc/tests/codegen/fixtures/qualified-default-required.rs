#![feature(no_core)]
#![no_core]
#![no_main]

trait Value {
    fn value(input: u32) -> u32;
}

impl Value for u32 {}

#[no_mangle]
pub extern "C" fn qualified_default_required(input: u32) -> u32 {
    <u32 as Value>::value(input)
}
