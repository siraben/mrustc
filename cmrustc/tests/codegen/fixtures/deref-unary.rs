#![feature(no_core)]
#![no_core]
#![no_main]

// alloc's `Box::into_raw_with_allocator`: `&raw mut **b` on a
// `ManuallyDrop<Box<T>>` -- the explicit `*` on a `Deref` ADT calls
// `deref` and reads through the reference it returns, rather than
// loading through the wrapper's value as if it were a pointer.
#[lang = "deref"]
trait Deref {
    type Target;
    fn deref(&self) -> &Self::Target;
}

struct Pair(u32, u32);

struct Wrap {
    tag: u32,
    inner: Pair,
}

impl Deref for Wrap {
    type Target = Pair;
    fn deref(&self) -> &Pair { &self.inner }
}

fn reborrow(w: &Wrap) -> u32 {
    let p: &Pair = &**w;
    p.0
}

#[no_mangle]
pub extern "C" fn deref_unary(x: u32) -> u32 {
    let w = Wrap { tag: 100, inner: Pair(x, x + 5) };
    reborrow(&w) + (*w).1 + w.tag
}
