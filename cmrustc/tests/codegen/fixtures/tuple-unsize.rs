#![feature(no_core)]
#![no_core]
#![no_main]

// core's `align_to` returns `(self, &[], &[])` where `(&[T], &[U], &[T])`
// is expected: each empty array unsizes at its own element, not the
// tuple.
#[rustc_intrinsic]
pub unsafe fn ptr_metadata<P: ?Sized>(ptr: *const P) -> usize;

impl<T> [T] {
    pub fn len(&self) -> usize {
        unsafe { ptr_metadata(self) }
    }
}

fn split(xs: &[u32], early: bool) -> (&[u32], &[u32], &[u32]) {
    if early {
        (xs, &[], &[])
    } else {
        (&[], xs, &[])
    }
}

#[no_mangle]
pub extern "C" fn tuple_unsize(x: u32) -> u32 {
    let arr = [x, x + 1, x + 2];
    let s: &[u32] = &arr;
    let (a, b, c) = split(s, true);
    let (d, e, f) = split(s, false);
    (a.len() * 100 + b.len() * 10 + c.len()) as u32
        + (d.len() * 100 + e.len() * 10 + f.len()) as u32 * 1000
}
