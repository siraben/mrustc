#![no_core]
#![feature(no_core)]
struct Timeval {
    sec: u32,
    #[cfg(not(windows))]
    usec: u32,
    #[cfg(windows)]
    usec: u16,
    #[cfg(windows)]
    only_windows: u64,
}
enum Shape {
    Point,
    Box {
        #[cfg(unix)]
        width: u32,
        #[cfg(windows)]
        width: u8,
        height: u32,
    },
}
#[no_mangle]
pub fn probe_cfg_decl_field(a: u32) -> u32 {
    let t = Timeval { sec: a, usec: 2 };
    let s = Shape::Box { width: 3, height: 4 };
    let extra = match s {
        Shape::Point => 0,
        Shape::Box { width, height } => width * height,
    };
    t.sec + t.usec + extra
}
