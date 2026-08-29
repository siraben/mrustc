#![feature(no_core)]
#![no_core]
#![no_main]

// Borrowing an array element (explicitly or as an autoref'd receiver)
// aliases the element, not a loaded copy — core's `buf[curr].write(d)`
// on a `[MaybeUninit<u8>; N]` digit buffer.
struct Cell {
    v: u32,
}

impl Cell {
    fn set(&mut self, v: u32) {
        self.v = v;
    }
}

struct Byte {
    b: u8,
}

impl Byte {
    fn put(&mut self, b: u8) {
        self.b = b;
    }
}

// A borrowed field aliases the field (core's `mem::replace(&mut
// self.start, n)` in `Range::spec_next`, and `self.cell.set(..)`).
struct Pair {
    start: u32,
    cell: Cell,
}

fn replace(dest: &mut u32, value: u32) -> u32 {
    let old = *dest;
    *dest = value;
    old
}

fn advance(p: &mut Pair) -> u32 {
    let old = replace(&mut p.start, p.start + 5);
    p.cell.set(old + 1);
    old
}

#[no_mangle]
pub extern "C" fn index_place(k: u32) -> u32 {
    let mut cells = [Cell { v: 0 }, Cell { v: 0 }, Cell { v: 0 }];
    cells[1].set(k);
    let mut bytes = [0u8; 4];
    let r = &mut bytes[2];
    *r = 7;
    let s: &mut [u8] = &mut bytes;
    let p = &mut s[0];
    *p = 1;
    let mut packed = [Byte { b: 0 }, Byte { b: 0 }];
    packed[1].put(k as u8 + 1);
    let mut pair = Pair { start: k, cell: Cell { v: 0 } };
    let first = advance(&mut pair);
    let second = advance(&mut pair);
    cells[1].v + bytes[2] as u32 * 10 + bytes[0] as u32 * 100
        + packed[1].b as u32 * 1000
        + (first + second + pair.start + pair.cell.v) * 100000
}
