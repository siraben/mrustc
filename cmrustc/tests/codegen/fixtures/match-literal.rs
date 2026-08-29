#![feature(no_core)]
#![no_core]
#![no_main]

// Literal patterns dispatch on the scrutinee's value.
fn classify(which: u32) -> u32 {
    match which {
        0 => 100,
        1 | 2 => 200,
        0x10 => 300,
        _ => 400,
    }
}

fn sign(v: i32) -> u32 {
    match v {
        -1 => 1,
        0 => 2,
        1 => 3,
        _ => 4,
    }
}

fn flag(b: bool) -> u32 {
    match b {
        true => 10,
        false => 20,
    }
}

fn letter(c: u8) -> u32 {
    match c {
        b'a' => 1,
        b'z' => 26,
        _ => 0,
    }
}

// Range patterns (with `x @` bindings and or-alternatives) are value
// tests, not switch keys (core's `LowerHex::digit`).
fn bucket(c: u8) -> u32 {
    match c {
        x @ b'0'..=b'9' => (x - b'0') as u32,
        b'a'..=b'f' => 10 + (c - b'a') as u32,
        b'A'..=b'F' | b'x' => 100,
        n if n > 200 => 300,
        _ => 200,
    }
}

#[no_mangle]
pub extern "C" fn match_literal(which: u32) -> u32 {
    classify(which) + sign(which as i32 - 1) + flag(which > 1) + letter(b'a' + which as u8)
        + bucket(b'a' + which as u8) * 1000 + bucket(b'0' + (which % 10) as u8) * 100000
        + bucket(b'x') + bucket(250)
}
