#![feature(no_core)]
#![no_core]
#![no_main]

// libc's `pub fn abort() -> !` / `pub fn exit(status: c_int) -> !`: the
// emitted C prelude already prototypes `void abort(void)`, and a
// never-returning host symbol has no value to forward.
#[lang = "sized"]
trait Sized {}

extern "C" {
    fn abort() -> !;
    fn exit(status: i32) -> !;
}

#[no_mangle]
pub extern "C" fn foreign_never(x: u32) -> u32 {
    if x > 200 {
        unsafe { abort() }
    }
    if x > 100 {
        unsafe { exit(0) }
    }
    x + 1
}
