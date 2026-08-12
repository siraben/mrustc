#![feature(no_core)]
#![no_core]
#![no_main]

// Production anchor: library/core/src/slice/rotate.rs:275.
fn const_min(left: usize, right: usize) -> usize {
    if right < left { right } else { left }
}

fn wrap_and_limit(left: usize, right: usize) -> usize {
    let wrapped: usize = left - right;
    let biased: usize = wrapped + 1;
    let limit: usize = 4294967301;
    const_min(biased, limit)
}

#[no_mangle]
pub extern "C" fn probe_usize(left: usize, right: usize) -> usize {
    wrap_and_limit(left, right)
}
