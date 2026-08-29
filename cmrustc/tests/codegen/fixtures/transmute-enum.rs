#![feature(no_core)]
#![no_core]
#![no_main]

// core's `ptr::Alignment`: a transparent wrapper around a fieldless enum
// whose discriminants are `1 << n`, built by `transmute::<usize, Alignment>`
// and read back with `self.0 as usize`. Enum values are blocks (slot 0 =
// variant index), so the transmute must build the block of the variant
// with that discriminant, and the cast must yield the discriminant.
#[rustc_intrinsic]
pub unsafe fn transmute<T, U>(x: T) -> U;

#[repr(usize)]
enum AlignEnum {
    Shl0 = 1 << 0,
    Shl1 = 1 << 1,
    Shl2 = 1 << 2,
    Shl3 = 1 << 3,
    Shl4 = 1 << 4,
}

#[repr(transparent)]
struct Align(AlignEnum);

impl Align {
    fn new(v: usize) -> Align { unsafe { transmute::<usize, Align>(v) } }
    fn get(self) -> usize { self.0 as usize }
    fn raw(self) -> usize { unsafe { transmute::<Align, usize>(self) } }
}

#[no_mangle]
pub extern "C" fn transmute_enum(v: usize) -> usize {
    let a = Align::new(v);
    let b = Align::new(v);
    a.get() * 100 + b.raw() + AlignEnum::Shl3 as usize
}
