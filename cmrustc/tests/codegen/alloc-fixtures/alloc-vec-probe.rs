#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write;

/// Pushes `count` numbers into a `Vec`, formats them into a `String`, and
/// reports the string's length plus the vector's sum.
#[no_mangle]
pub extern "C" fn probe_alloc_vec(count: u32) -> u32 {
    let mut v: Vec<u32> = Vec::new();
    let mut i = 0;
    while i < count {
        v.push(i * 3);
        i += 1;
    }
    let mut s = String::new();
    let mut sum = 0;
    for x in v.iter() {
        sum += *x;
        let _ = write!(s, "{},", x);
    }
    s.len() as u32 * 1000 + sum
}
