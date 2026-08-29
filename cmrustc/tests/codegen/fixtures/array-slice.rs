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

// The unsize inside a const-generic fn takes N from the instance.
fn count<const N: usize>(a: &[u32; N]) -> u32 {
    let s: &[u32] = a;
    s.len() as u32 + s[0]
}

// A pointer *to* a fat reference is thin: casting it to `*const ()` and
// back must keep the reference intact (core's `Argument::new` does
// `NonNull::from_ref(x).cast()` with `x: &&str`).
fn through_unit(s: &[u32]) -> u32 {
    let p: *const &[u32] = &s;
    let q = p as *const ();
    let back: &[u32] = unsafe { *(q as *const &[u32]) };
    back[1] + back.len() as u32
}

#[no_mangle]
pub extern "C" fn array_slice(k: u32) -> u32 {
    let a = [1u32, 2, 3, k];
    let b = [k; 3];
    let mut c = [0u8; 4];
    c[1] = 9;
    c[3] = c[1] + 1;
    sum(&a) + a[2] + sum(&b) + c[3] as u32 + count(&a) + through_unit(&a)
}
