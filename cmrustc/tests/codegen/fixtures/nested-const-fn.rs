#![feature(no_core)]
#![no_core]
#![no_main]

// A `const unsafe fn` nested in a body is an item, not a `const {}` block
// (core's `align_offset` keeps `const unsafe fn mod_inv`).
#[no_mangle]
pub extern "C" fn nested_const_fn(x: u32) -> u32 {
    const unsafe fn twice(v: u32) -> u32 {
        const TABLE: [u32; 2] = [2, 3];
        v * TABLE[0]
    }
    const fn plus(v: u32) -> u32 {
        v + 1
    }
    unsafe { twice(x) + plus(x) }
}
