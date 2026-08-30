#![feature(no_core)]
#![no_core]
#![no_main]

#[lang = "sized"]
trait Sized {}

// `let v: Vec<i64> = ...`: an annotated path with fewer arguments than
// parameters takes the declared defaults (`A = Global`).
struct Global;

trait Mk {
    fn mk() -> Self;
    fn tag(&self) -> u32;
}

impl Mk for Global {
    fn mk() -> Global { Global }
    fn tag(&self) -> u32 { 40 }
}

struct Wrap<T, A = Global> {
    value: T,
    alloc: A,
}

fn make<A: Mk>(value: u32) -> Wrap<u32, A> {
    Wrap { value: value, alloc: A::mk() }
}

#[no_mangle]
pub extern "C" fn param_default(x: u32) -> u32 {
    // `A` is reachable only through the default.
    let w: Wrap<u32> = make(x);
    w.value + w.alloc.tag()
}
