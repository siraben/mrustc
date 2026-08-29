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

#[rustc_intrinsic]
pub unsafe fn ptr_metadata<P: ?Sized>(ptr: *const P) -> usize;

fn slice_len(s: &[u32]) -> usize {
    unsafe { ptr_metadata(s) }
}

fn nested(x: u32) -> u32 {
    // A body-local const, used as an array length.
    const LEN: usize = 3;
    let a = [x; LEN];
    // A length the typechecker cannot fold: the block header carries it
    // into the slice.
    const LEN2: usize = 2 + 2;
    let b = [x; LEN2];
    a[0] + a[2] + LEN as u32 + slice_len(&b) as u32
}

#[no_mangle]
pub extern "C" fn const_sum(x: u32) -> u32 {
    let s = S;
    if FLAG { x + K + S::M + s.get() + nested(x) } else { 0 }
}
