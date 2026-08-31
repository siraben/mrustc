#![feature(no_core)]
#![no_core]
#![no_main]

struct Point {
    x: u32,
    y: u32,
}

#[lang = "owned_box"]
struct Box<T: ?Sized>(*mut T);

#[rustc_intrinsic]
fn box_new<T>(value: T) -> Box<T>;

#[lang = "deref"]
trait Deref {
    type Target;
    fn deref(&self) -> &Self::Target;
}

impl<T: ?Sized> Deref for Box<T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.0 }
    }
}

#[no_mangle]
pub extern "C" fn box_field_deref(x: u32, y: u32) -> u32 {
    let point = box_new(Point { x, y });
    point.x * 10 + point.y
}
