#![feature(no_core)]
#![no_core]
#![no_main]

// alloc's `Box::into_raw_with_allocator`: `ptr::read(&b.1)` where `b` is a
// `ManuallyDrop<Box<T, A>>` — a tuple-field access reaches the tuple struct
// through the wrapper's `Deref`, as a named field does.
#[lang = "deref"]
trait Deref {
    type Target;
    fn deref(&self) -> &Self::Target;
}

struct Pair(u32, u32);

struct Wrap {
    inner: Pair,
}

impl Deref for Wrap {
    type Target = Pair;
    fn deref(&self) -> &Pair { &self.inner }
}

fn second(w: &Wrap) -> u32 {
    w.1
}

#[no_mangle]
pub extern "C" fn tuple_field_deref(x: u32) -> u32 {
    let w = Wrap { inner: Pair(x, x + 5) };
    second(&w) + w.0
}
