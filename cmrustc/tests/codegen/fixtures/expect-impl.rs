#![feature(no_core)]
#![no_core]
#![no_main]

// std's `set_current_info(.., Some(Box::from("main")))` into an
// `Option<Box<str>>` slot: both `From<T> for Box<T>` and `From<&str> for
// Box<str>` accept the argument; the expected type picks the impl, and
// it reaches the inner call only by typing the argument again.
#[lang = "sized"]
trait Sized {}

trait From<T> {
    fn from(t: T) -> Self;
}

struct Bx<T>(T);

impl<T> From<T> for Bx<T> {
    fn from(t: T) -> Bx<T> { Bx(t) }
}

impl From<u32> for Bx<u64> {
    fn from(t: u32) -> Bx<u64> { Bx((t as u64) * 2) }
}

enum Opt<T> {
    Some(T),
    None,
}

fn take(o: Opt<Bx<u64>>) -> u64 {
    match o {
        Opt::Some(b) => b.0,
        Opt::None => 0,
    }
}

fn direct(b: Bx<u64>) -> u64 {
    b.0
}

#[no_mangle]
pub extern "C" fn expect_impl(x: u32) -> u32 {
    (take(Opt::Some(Bx::from(x))) + direct(Bx::from(x))) as u32
}
