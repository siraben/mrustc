#![feature(no_core)]
#![no_core]
#![no_main]

// rustc probes by value before autoref: `r.ts()` on `r: &Item` is
// `<Item as ToS>::ts(&self)` (self type = the receiver), not the blanket
// with `T = &Item`; likewise `r.sp()` reaches the blanket impl for `Item`,
// not `impl Spec for &Item` (alloc's `SpecToString for &str` on a `&str`).
struct Item {
    v: u32,
}

trait Disp {}
impl Disp for Item {}
impl<T: Disp + ?Sized> Disp for &T {}

trait Spec {
    fn sp(&self) -> u32;
}
impl<T: Disp + ?Sized> Spec for T {
    default fn sp(&self) -> u32 {
        1
    }
}
impl Spec for &Item {
    fn sp(&self) -> u32 {
        2
    }
}

trait ToS {
    fn ts(&self) -> u32;
}
impl<T: Disp + ?Sized> ToS for T {
    fn ts(&self) -> u32 {
        <Self as Spec>::sp(self)
    }
}

#[no_mangle]
pub extern "C" fn byvalue_first(x: u32) -> u32 {
    let it = Item { v: x };
    let r: &Item = &it;
    r.ts() * 10 + r.sp()
}
