#![feature(no_core)]
#![no_core]
#![no_main]

// Two impls of one trait for the same Self, differing only in the trait
// argument: the call must select by the trait argument (core's
// `impl SliceIndex<ByteStr> for usize` precedes `impl SliceIndex<[T]>`).
struct Bytes {
    data: [u8; 4],
}

trait Index<S: ?Sized> {
    type Out;
    fn get(self, s: &S) -> Self::Out;
}

impl Index<Bytes> for usize {
    type Out = u32;
    fn get(self, s: &Bytes) -> u32 {
        s.data[self] as u32 + 1000
    }
}

impl<T: Copy> Index<[T]> for usize {
    type Out = T;
    fn get(self, s: &[T]) -> T {
        s[self]
    }
}

trait Copy {}
impl Copy for u32 {}
impl Copy for u8 {}

fn lookup<T: Copy, I: Index<[T]>>(s: &[T], i: I) -> I::Out {
    i.get(s)
}

fn lookup_bytes<I: Index<Bytes>>(b: &Bytes, i: I) -> I::Out {
    i.get(b)
}

#[no_mangle]
pub extern "C" fn trait_args(i: u32) -> u32 {
    let table: [u32; 4] = [10, 20, 30, 40];
    let b = Bytes { data: [1, 2, 3, 4] };
    let idx = i as usize;
    // An index that is only a literal: rustc infers `usize` from the
    // `Index<[T]>` bound; the call must still select the `[T]` impl.
    let mut plain = 0;
    if i > 100 {
        plain = 1;
    }
    lookup(&table, idx) + lookup_bytes(&b, idx) + lookup(&table, plain)
}
