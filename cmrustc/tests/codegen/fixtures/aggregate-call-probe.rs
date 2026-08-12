#![feature(no_core)]
#![no_core]
#![no_main]

struct Outer {
    inner: Inner,
    tag: u32,
}

struct Inner {
    first: u32,
    second: u32,
}

fn select(value: Outer, bias: u32) -> u32 {
    value.inner.second + bias
}

#[no_mangle]
pub extern "C" fn probe_aggregate_call(x: u32) -> u32 {
    let bias: u32 = 3u32;
    select(
        Outer {
            inner: Inner { first: x, second: x + 1u32 },
            tag: x + 2u32,
        },
        bias,
    )
}
