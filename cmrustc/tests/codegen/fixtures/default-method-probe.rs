#![feature(no_core)]
#![no_core]
#![no_main]

trait Twice {
    fn one(&self) -> u32;
    fn twice(&self) -> u32 {
        self.one() * 2u32
    }
}

impl Twice for u32 {
    fn one(&self) -> u32 {
        *self
    }
}

#[no_mangle]
pub extern "C" fn probe_default(value: u32) -> u32 {
    value.twice()
}
