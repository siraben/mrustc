#![feature(no_core)]
#![no_core]
#![no_main]

// Consts used as values evaluate their initializer bodies.
const K: u32 = 7;
const FLAG: bool = true;

struct S;

impl S {
    const M: u32 = 5;
    fn get(&self) -> u32 {
        Self::M + K
    }
}

fn nested(x: u32) -> u32 {
    // A body-local const, used as an array length.
    const LEN: usize = 3;
    let a = [x; LEN];
    a[0] + a[2] + LEN as u32
}

#[no_mangle]
pub extern "C" fn const_sum(x: u32) -> u32 {
    let s = S;
    if FLAG { x + K + S::M + s.get() + nested(x) } else { 0 }
}
