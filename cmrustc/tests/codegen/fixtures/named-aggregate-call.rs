#![feature(no_core)]
#![no_core]
#![no_main]

struct Pair {
    first: u32,
    second: u32,
}

fn make_pair(x: u32) -> Pair {
    Pair { first: x, second: x + 1u32 }
}

#[no_mangle]
pub extern "C" fn rejected_aggregate_call(x: u32) -> u32 {
    make_pair(x).second
}
