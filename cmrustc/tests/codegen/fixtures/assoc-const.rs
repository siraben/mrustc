#![feature(no_core)]
#![no_core]
#![no_main]

// A trait's associated const resolves through Self / a type parameter to
// the impl's item (core's `GenericRadix::BASE` inside `fmt_int`).
trait Radix {
    const BASE: u32;
    fn digits(mut x: u32) -> u32 {
        let mut n = 0;
        while x > 0 {
            x = x / Self::BASE;
            n += 1;
        }
        n
    }
}

struct Dec;
struct Hex;

impl Radix for Dec {
    const BASE: u32 = 10;
}

impl Radix for Hex {
    const BASE: u32 = 16;
}

fn count<R: Radix>(x: u32) -> u32 {
    R::digits(x) + R::BASE
}

#[no_mangle]
pub extern "C" fn assoc_const(x: u32) -> u32 {
    count::<Dec>(x) * 100 + count::<Hex>(x)
}
