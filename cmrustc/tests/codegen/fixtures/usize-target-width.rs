#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn target_width(left: usize, right: usize) -> usize {
    if right < left { right + 1 } else { left - 1 }
}
