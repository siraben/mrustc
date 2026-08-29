#![feature(no_core)]
#![no_core]
#![no_main]

// std's `self.inner.borrow_mut().write_all(buf)`: the guard a call
// produces as a `&mut self` receiver is a temporary; its drop glue (the
// RefMut's BorrowRefMut resets the borrow flag) runs after the call.
#[lang = "sized"]
trait Sized {}
#[lang = "drop"]
trait Drop {
    fn drop(&mut self);
}

static mut BORROWS: u32 = 0;
static mut DROPS: u32 = 0;

struct Flag {
    depth: u32,
}

impl Drop for Flag {
    fn drop(&mut self) {
        unsafe { BORROWS = BORROWS - 1; DROPS = DROPS + 1; }
    }
}

// The Drop lives on a field, as RefMut's does.
struct Guard {
    value: u32,
    flag: Flag,
}

impl Guard {
    fn poke(&mut self, x: u32) -> u32 { self.value + x }
}

struct Cell {
    value: u32,
}

impl Cell {
    fn lock(&self) -> Guard {
        unsafe { BORROWS = BORROWS + 1; }
        Guard { value: self.value, flag: Flag { depth: 1 } }
    }
}

#[no_mangle]
pub extern "C" fn temp_drop(x: u32) -> u32 {
    let cell = Cell { value: 100 };
    let a = cell.lock().poke(x);
    let b = cell.lock().poke(x + 1);
    unsafe { a + b + BORROWS * 1000 + DROPS * 10000 }
}
