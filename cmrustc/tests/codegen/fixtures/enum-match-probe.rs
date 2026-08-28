#![feature(no_core)]
#![no_core]
#![no_main]

enum Shape {
    Point,
    Square(u32),
    Rect { w: u32, h: u32 },
}

fn area(shape: Shape) -> u32 {
    match shape {
        Shape::Point => 0u32,
        Shape::Square(side) => side * side,
        Shape::Rect { w, h } => w * h,
    }
}

#[no_mangle]
pub extern "C" fn probe_point() -> u32 {
    area(Shape::Point)
}

#[no_mangle]
pub extern "C" fn probe_square(side: u32) -> u32 {
    area(Shape::Square(side))
}

#[no_mangle]
pub extern "C" fn probe_rect(w: u32, h: u32) -> u32 {
    area(Shape::Rect { w: w, h: h })
}
