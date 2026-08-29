#![feature(no_core)]
#![no_core]
#![no_main]

// core's NonZero::new transmutes the integer straight to
// Option<NonZero<T>> (niche layout); the shim builds a real Option.
enum Option<T> {
    None,
    Some(T),
}

struct NonZero(u32);

#[rustc_intrinsic]
pub unsafe fn transmute_unchecked<A, B>(a: A) -> B;

fn new(n: u32) -> Option<NonZero> {
    unsafe { transmute_unchecked(n) }
}

fn back(o: Option<NonZero>) -> u32 {
    unsafe { transmute_unchecked(o) }
}

#[no_mangle]
pub extern "C" fn niche_option(n: u32) -> u32 {
    let tag = match new(n) {
        Option::Some(nz) => nz.0 + 100,
        Option::None => 1,
    };
    tag + back(new(n)) * 1000
}
