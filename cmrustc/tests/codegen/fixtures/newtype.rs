#![feature(no_core)]
#![no_core]
#![no_main]

// Single-field structs are transparent: a newtype and its field share a
// representation, so pointer casts between them agree.
struct Ptr {
    pointer: *const u32,
}

struct Wrap(u32);

impl Ptr {
    fn as_ptr(self) -> *const u32 {
        self.pointer
    }
}

fn read(p: Ptr) -> u32 {
    unsafe { *p.as_ptr() }
}

#[no_mangle]
pub extern "C" fn newtype(x: u32) -> u32 {
    let mut w = Wrap(x);
    w.0 = w.0 + 1;
    let rp: *const u32 = &w as *const Wrap as *const u32;
    let p = unsafe { *(&rp as *const *const u32 as *const Ptr) };
    let Wrap(inner) = w;
    read(p) + inner
}
