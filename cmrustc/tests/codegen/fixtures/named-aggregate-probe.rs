#![feature(no_core)]
#![no_core]
#![no_main]

struct Outer {
    inner: Inner,
    tail: u32,
}

struct Inner {
    signed: i32,
    first: u32,
    second: u32,
}

#[no_mangle]
pub extern "C" fn probe_aggregate(x: u32) -> u32 {
    Outer {
        tail: x + 3u32,
        inner: Inner {
            second: x + 2u32,
            signed: 17i32,
            first: x + 1u32,
        },
    }.inner.second
}
