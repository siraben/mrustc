#![feature(no_core)]
#![no_core]
#![no_main]

use provider::bounded;

#[no_mangle]
pub extern "C" fn consumer_probe(x: u32) -> u32 {
    bounded::<u32>(x)
}
