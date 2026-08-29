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

#[no_mangle]
pub extern "C" fn const_sum(x: u32) -> u32 {
    let s = S;
    if FLAG { x + K + S::M + s.get() } else { 0 }
}
