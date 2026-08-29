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

// Const paths as match patterns are value tests against the constant
// (core's `FormattingOptions::get_align`: `match flags & ALIGN_BITS {
// ALIGN_LEFT => .., ALIGN_RIGHT => .., .. }`).
mod flags {
    pub const ALIGN_BITS: u32 = 3 << 29;
    pub const ALIGN_LEFT: u32 = 0 << 29;
    pub const ALIGN_RIGHT: u32 = 1 << 29;
    pub const ALIGN_CENTER: u32 = 2 << 29;
}

fn align(f: u32) -> u32 {
    match f & flags::ALIGN_BITS {
        flags::ALIGN_LEFT => 1,
        flags::ALIGN_RIGHT => 2,
        flags::ALIGN_CENTER => 3,
        _ => 4,
    }
}

// Nested variant / literal sub-patterns are conjunctive tests under the
// outer key (core's `FormattingOptions::align`: `match align {
// Some(Alignment::Left) => .., Some(Alignment::Right) => .., .. }`).
enum Align { Left, Right, Center }
enum Opt<T> { None, Some(T) }

fn align_bits(a: Opt<Align>) -> u32 {
    match a {
        Opt::Some(Align::Left) => 1,
        Opt::Some(Align::Right) => 2,
        Opt::Some(Align::Center) => 3,
        Opt::None => 4,
    }
}

fn nested(o: Opt<u32>) -> u32 {
    match o {
        Opt::Some(3) => 30,
        Opt::Some(x @ 10..=20) => x,
        Opt::Some(_) => 1,
        Opt::None => 0,
    }
}

#[no_mangle]
pub extern "C" fn match_literal(which: u32) -> u32 {
    classify(which) + sign(which as i32 - 1) + flag(which > 1) + letter(b'a' + which as u8)
        + bucket(b'a' + which as u8) * 1000 + bucket(b'0' + (which % 10) as u8) * 100000
        + bucket(b'x') + bucket(250)
        + align(which << 29) * 10000000
        + align_bits(match which { 0 => Opt::Some(Align::Left), 1 => Opt::Some(Align::Right), 2 => Opt::Some(Align::Center), _ => Opt::None }) * 100000000
        + nested(Opt::Some(which + 1)) * 2
}
