#![no_core]
#![feature(no_core)]
#[no_mangle]
pub fn probe_local_type(a: usize) -> usize {
    type Chunk = usize;
    let x: Chunk = a;
    let y = x + 1;
    y
}
