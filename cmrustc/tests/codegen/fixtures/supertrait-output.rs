#![feature(no_core)]
#![no_core]
#![no_main]

// core's `trait DisplayInt: Rem<Output = Self> + ..` and `fmt_int<T:
// DisplayInt>`: `(x % base).to_u8()` has the projection type
// `<T as Rem>::Output`, which must normalize to `T` through the
// supertrait's equality — an unnormalized projection matched any inherent
// `to_u8` (core's `ascii::Char::to_u8`), so the wrong callee ran.
trait Rem<Rhs = Self> {
    type Output;
    fn rem(self, rhs: Rhs) -> Self::Output;
}

impl Rem for u32 {
    type Output = u32;
    fn rem(self, rhs: u32) -> u32 { self % rhs }
}

trait DisplayInt: Rem<Output = Self> {
    fn to_u8(&self) -> u8;
}

impl DisplayInt for u32 {
    fn to_u8(&self) -> u8 { *self as u8 }
}

enum Decoy { A, B }

impl Decoy {
    fn to_u8(&self) -> u8 { 200 }
}

fn last_digit<T: DisplayInt>(x: T, base: T) -> u8 {
    let d = x.rem(base);
    d.to_u8()
}

#[no_mangle]
pub extern "C" fn supertrait_output(x: u32) -> u8 {
    let keep = Decoy::B;
    last_digit(x, 10) + keep.to_u8()
}
