#![no_core]
#![feature(no_core)]
macro_rules! declare_host {
    ($name:ident) => {
        pub safe fn $name(a: u32, b: u32) -> u32;
    };
}
unsafe extern "C" {
    declare_host!(host_add);
}
#[no_mangle]
pub fn probe_extern_block_macro(a: u32) -> u32 {
    host_add(a, 2)
}
