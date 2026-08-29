#![feature(no_core)]
#![no_core]
#![no_main]

// Scalar arrays pack their elements; `&[T; N] -> &[T]` builds a
// [block, N] pair that slice code indexes at element width.
#[rustc_intrinsic]
pub unsafe fn ptr_metadata<P: ?Sized>(ptr: *const P) -> usize;

impl<T> [T] {
    pub fn len(&self) -> usize {
        unsafe { ptr_metadata(self) }
    }
}

fn sum(xs: &[u32]) -> u32 {
    let mut i = 0;
    let mut s = 0;
    while i < xs.len() {
        s = s + xs[i];
        i = i + 1;
    }
    s
}

#[no_mangle]
pub extern "C" fn array_slice(k: u32) -> u32 {
    let a = [1u32, 2, 3, k];
    let b = [k; 3];
    let mut c = [0u8; 4];
    c[1] = 9;
    c[3] = c[1] + 1;
    sum(&a) + a[2] + sum(&b) + c[3] as u32
}
