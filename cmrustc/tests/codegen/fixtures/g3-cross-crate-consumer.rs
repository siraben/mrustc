#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn consumer_probe(x: u32) -> u32 {
    provider::bounded::<u32>(x)
}
