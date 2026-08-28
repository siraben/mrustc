#![feature(no_core)]
#![no_core]
#![no_main]

trait Scale {
    fn scale(self) -> u32;
}

impl Scale for u32 {
    fn scale(self) -> u32 { self * 2u32 }
}

impl Scale for u8 {
    fn scale(self) -> u32 { (self as u32) * 3u32 }
}

fn apply<T: Scale>(value: T) -> u32 {
    value.scale()
}

#[no_mangle]
pub extern "C" fn probe_u32(value: u32) -> u32 {
    apply(value)
}

#[no_mangle]
pub extern "C" fn probe_u8(value: u8) -> u32 {
    apply(value)
}
