#![feature(no_core)]
#![no_core]
#![no_main]

// std's BufWriter::flush_buf: `&self.buffer[self.written..]` on a
// `Vec<u8>` -- a Vec-like ADT indexes through its Deref to the slice.
#[rustc_intrinsic]
pub unsafe fn ptr_metadata<P: ?Sized>(ptr: *const P) -> usize;

#[lang = "deref"]
trait Deref {
    type Target;
    fn deref(&self) -> &Self::Target;
}

pub mod ops {
    pub struct RangeFrom<Idx> { pub start: Idx }
    pub struct RangeTo<Idx> { pub end: Idx }
}

impl<T> [T] {
    pub fn len(&self) -> usize {
        unsafe { ptr_metadata(self) }
    }
}

struct Vec {
    data: [u32; 4],
    len: usize,
}

impl Deref for Vec {
    type Target = [u32];
    fn deref(&self) -> &[u32] { &self.data[..self.len] }
}

fn tail_len(b: &Vec, n: usize) -> usize {
    b[n..].len()
}

fn tail_first(b: &Vec, n: usize) -> u32 {
    let t: &[u32] = &b[n..];
    t[0]
}

fn element(b: &Vec, i: usize) -> u32 {
    b[i]
}

fn slice_second(values: &[u32]) -> u32 {
    values[1]
}

#[no_mangle]
pub extern "C" fn vec_index(x: u32) -> u32 {
    let b = Vec { data: [x, x + 1, x + 2, 99], len: 3 };
    tail_len(&b, 1) as u32 * 100 + tail_first(&b, 2)
        + element(&b, 1) * 1000 + slice_second(&b) * 10000
}
