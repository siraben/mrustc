#![feature(no_core)]
#![no_core]
#![no_main]

// Overlapping trait impls told apart by an argument's shape: a method
// written for any `I` only stands in when no impl is written for the
// argument's own type (alloc's `SpecExtend<&T, slice::Iter<T>>` beats
// `SpecExtend<T, I>`; the generic one would iterate references).
trait Ext<A, I> {
    fn ext(&mut self, items: I) -> u32;
}

struct Cursor {
    a: u32,
    b: u32,
    c: u32,
}

struct Holder {
    total: u32,
    calls: u32,
}

impl<I> Ext<u32, I> for Holder {
    fn ext(&mut self, _items: I) -> u32 {
        self.calls = self.calls + 100;
        100
    }
}

impl Ext<u32, Cursor> for Holder {
    fn ext(&mut self, items: Cursor) -> u32 {
        self.total = self.total + items.a + items.b + items.c;
        self.calls = self.calls + 1;
        self.total
    }
}

#[no_mangle]
pub extern "C" fn spec_extend(x: u32) -> u32 {
    let mut h = Holder { total: 0, calls: 0 };
    let c = Cursor { a: x, b: 2, c: 3 };
    let r = h.ext(c);
    let s = h.ext(7u8);
    r + s * 1000 + h.calls * 100000
}
