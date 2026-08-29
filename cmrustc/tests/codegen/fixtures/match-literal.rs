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

#[no_mangle]
pub extern "C" fn match_literal(which: u32) -> u32 {
    classify(which) + sign(which as i32 - 1) + flag(which > 1) + letter(b'a' + which as u8)
}
