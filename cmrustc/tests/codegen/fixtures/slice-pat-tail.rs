#![feature(no_core)]
#![no_core]
#![no_main]

// core's `<[T]>::last`: `if let [.., last] = self { Some(last) } else
// { None }` -- elements after a slice pattern's rest are `len - k`.
#[rustc_intrinsic]
pub unsafe fn ptr_metadata<P: ?Sized>(ptr: *const P) -> usize;

impl<T> [T] {
    pub fn len(&self) -> usize {
        unsafe { ptr_metadata(self) }
    }
}

fn last(xs: &[u32]) -> u32 {
    if let [.., last] = xs { *last } else { 100 }
}

fn ends(xs: &[u32]) -> u32 {
    match xs {
        [first, .., last] => *first * 10 + *last,
        [only] => *only * 1000,
        [] => 7,
    }
}

fn second_last(xs: &[u32]) -> u32 {
    if let [.., a, _] = xs { *a } else { 200 }
}

#[no_mangle]
pub extern "C" fn slice_pat_tail(x: u32) -> u32 {
    let arr = [x, x + 1, x + 2];
    let one = [x + 5];
    let none: [u32; 0] = [];
    let s: &[u32] = &arr;
    let o: &[u32] = &one;
    let e: &[u32] = &none;
    last(s) + last(e) + ends(s) + ends(o) + ends(e) + second_last(s) + second_last(o)
}
