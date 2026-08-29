#![feature(no_core)]
#![no_core]
#![no_main]

// core's memrchr: `text[offset..].iter()` -- a range index is a
// subslice ([data + start, len]) rather than an element.
#[rustc_intrinsic]
pub unsafe fn ptr_metadata<P: ?Sized>(ptr: *const P) -> usize;

pub mod ops {
    pub struct Range<Idx> { pub start: Idx, pub end: Idx }
    pub struct RangeFrom<Idx> { pub start: Idx }
    pub struct RangeTo<Idx> { pub end: Idx }
    pub struct RangeFull;
}

impl<T> [T] {
    pub fn len(&self) -> usize {
        unsafe { ptr_metadata(self) }
    }
    pub fn first_or(&self, d: T) -> T where T: Copy {
        if self.len() == 0 { d } else { self[0] }
    }
}

#[lang = "copy"]
trait Copy {}
impl Copy for u32 {}

fn tail_len(xs: &[u32], n: usize) -> usize {
    xs[n..].len()
}

fn tail_first(xs: &[u32], n: usize) -> u32 {
    xs[n..].first_or(99)
}

fn mid(xs: &[u32], a: usize, b: usize) -> u32 {
    let m: &[u32] = &xs[a..b];
    m.len() as u32 * 10 + m.first_or(0)
}

fn head(xs: &[u32], b: usize) -> u32 {
    let h = &xs[..b];
    h.len() as u32
}

#[no_mangle]
pub extern "C" fn range_index(x: u32) -> u32 {
    let arr = [x, x + 1, x + 2, x + 3];
    let s: &[u32] = &arr;
    tail_len(s, 1) as u32 + tail_first(s, 2) + tail_first(s, 4)
        + mid(s, 1, 3) + head(s, 2)
}
