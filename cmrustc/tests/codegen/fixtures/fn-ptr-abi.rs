#![feature(no_core)]
#![no_core]
#![no_main]

// libc shapes: an `extern "C" fn(..)` pointer type (with a named
// parameter), and a C-variadic foreign declaration.
extern "C" {
    fn host_sum(count: u32, ...) -> u32;
}

type Adder = extern "C" fn(x: u32) -> u32;

extern "C" fn plus_one(x: u32) -> u32 {
    x + 1
}

fn apply(f: Adder, v: u32) -> u32 {
    f(v)
}

#[no_mangle]
pub extern "C" fn fn_ptr_abi(x: u32) -> u32 {
    let total = unsafe { host_sum(3, x, 10, 100) };
    apply(plus_one, x) + total * 1000
}
