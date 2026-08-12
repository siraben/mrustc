#![feature(no_core)]
#![no_core]
#![no_main]

trait Convert {
    fn convert(value: u32) -> u32;
}

impl Convert for u32 {
    fn convert(value: u32) -> u32 {
        value
    }
}

#[no_mangle]
pub extern "C" fn probe(x: u32) -> u32 {
    x
}
