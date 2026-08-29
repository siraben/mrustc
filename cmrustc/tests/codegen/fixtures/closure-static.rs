#![feature(no_core)]
#![no_core]
#![no_main]

// thread_local!'s key: `LocalKey::new(const { if needs_drop { |init| { static
// VAL: .. = ..; VAL.get(init) } else { |init| .. } })` -- a static declared
// inside a closure inside a static's initializer, reached through a call
// argument; the closures coerce to the fn pointer per branch, and VAL's
// own body sees `__INIT`, declared in the enclosing block.
struct Cell {
    value: u32,
}

impl Cell {
    const fn new(v: u32) -> Cell { Cell { value: v } }
    fn get(&self) -> u32 { self.value }
}

struct Key {
    inner: fn(u32) -> u32,
}

impl Key {
    const fn new(inner: fn(u32) -> u32) -> Key { Key { inner } }
    fn with(&self, x: u32) -> u32 { (self.inner)(x) }
}

static KEY: Key = {
    const __INIT: Cell = Cell::new(40);
    Key::new(const {
        if true {
            |init| {
                static VAL: Cell = __INIT;
                VAL.get() + init
            }
        } else {
            |init| init
        }
    })
};

#[no_mangle]
pub extern "C" fn closure_static(x: u32) -> u32 {
    KEY.with(x)
}
