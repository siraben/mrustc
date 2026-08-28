#![feature(no_core)]
#![no_core]
#![no_main]

fn sum_to(n: u32) -> u32 {
    let mut total: u32 = 0u32;
    let mut i: u32 = 0u32;
    while i < n {
        total = total + i;
        i = i + 1u32;
    }
    total
}

fn count_down(start: u32) -> u32 {
    let mut steps: u32 = 0u32;
    let mut value: u32 = start;
    loop {
        if value == 0u32 {
            break;
        }
        value = value - 1u32;
        steps = steps + 1u32;
    }
    steps
}

#[no_mangle]
pub extern "C" fn probe_sum(n: u32) -> u32 {
    sum_to(n)
}

#[no_mangle]
pub extern "C" fn probe_count(start: u32) -> u32 {
    count_down(start)
}
