#![feature(no_core)]
#![no_core]
#![no_main]

struct Pair([u32; 2]);

fn sum(Pair([left, right]): Pair) -> u32 {
    left + right
}

#[no_mangle]
pub extern "C" fn newtype_array_param_probe() -> u32 {
    sum(Pair([19, 23]))
}
