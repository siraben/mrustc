#![no_core]
#![feature(no_core)]
extern "C" {
    pub static HOST_LEVEL: u32;
}
#[no_mangle]
pub fn probe_foreign_static(a: u32) -> u32 {
    let level = unsafe { HOST_LEVEL };
    let p: *const u32 = unsafe { &HOST_LEVEL };
    let again = unsafe { *p };
    level + again + a
}
