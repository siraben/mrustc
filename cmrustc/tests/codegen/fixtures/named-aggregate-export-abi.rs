#![feature(no_core)]
#![no_core]
#![no_main]

struct Pair {
    first: u32,
    second: u32,
}

#[no_mangle]
pub extern "C" fn rejected_aggregate_abi(pair: Pair) -> u32 {
    pair.second
}
