#![feature(no_core)]
#![no_core]
#![no_main]

// A function declared in an extern block forwards to the C symbol of
// that name (alloc's `__rust_alloc` family is supplied by the host).
unsafe extern "C" {
    fn host_add(a: u32, b: u32) -> u32;
    fn host_scale(v: u32) -> u32;
}

#[no_mangle]
pub extern "C" fn extern_forward(k: u32) -> u32 {
    unsafe { host_add(k, 5) + host_scale(k) * 100 }
}
