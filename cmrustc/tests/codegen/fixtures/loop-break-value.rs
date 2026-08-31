#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn loop_break_value(mut n: u32) -> u32 {
    let (left, right) = loop {
        if n > 3 {
            break (n, n + 7);
        }
        n += 1;
    };
    left * 100 + right
}
