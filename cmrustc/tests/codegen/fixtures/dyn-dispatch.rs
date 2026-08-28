#![feature(no_core)]
#![no_core]
#![no_main]

// `&T -> &dyn Trait` builds a [data, vtable] pair; calls through the
// trait object index the vtable (default methods included).
trait Shape {
    fn area(&self) -> u32;
    fn scaled(&self, k: u32) -> u32 {
        self.area() * k
    }
}

struct Sq(u32);
struct Rect {
    w: u32,
    h: u32,
}

impl Shape for Sq {
    fn area(&self) -> u32 {
        self.0 * self.0
    }
}

impl Shape for Rect {
    fn area(&self) -> u32 {
        self.w * self.h
    }
    fn scaled(&self, k: u32) -> u32 {
        self.area() * k + 1
    }
}

fn measure(s: &dyn Shape, k: u32) -> u32 {
    s.scaled(k) + s.area()
}

#[no_mangle]
pub extern "C" fn dyn_area(sel: u32, k: u32) -> u32 {
    let sq = Sq(3);
    let r = Rect { w: 2, h: 5 };
    if sel == 0 { measure(&sq, k) } else { measure(&r, k) }
}
