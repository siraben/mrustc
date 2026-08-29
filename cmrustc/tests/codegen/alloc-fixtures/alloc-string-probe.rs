#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

/// Builds strings with `format!`, `to_string`, `push`/`push_str` and `+`,
/// boxes a value, collects into a `Vec<String>`, and reports a checksum:
/// total bytes * 1000 + sum of the first bytes + boxed value.
#[no_mangle]
pub extern "C" fn probe_alloc_string(count: u32) -> u32 {
    let mut parts: Vec<String> = Vec::new();
    let mut i = 0;
    while i < count {
        let s = format!("{}-{:x}", i, i * 16);
        parts.push(s);
        i += 1;
    }
    let mut joined = String::new();
    for p in parts.iter() {
        joined.push_str(p.as_str());
        joined.push(';');
    }
    let tail = count.to_string() + "!";
    let all = joined + &tail;
    let boxed: Box<u32> = Box::new(count * 7);
    let mut first_bytes = 0u32;
    for p in parts.iter() {
        first_bytes += p.as_bytes()[0] as u32;
    }
    all.len() as u32 * 1000 + first_bytes + *boxed
}
