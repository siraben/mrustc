#![feature(no_core)]
#![no_core]
#![no_main]

#[lang = "sized"]
trait Sized {}

// std's `lang_start`: `__rust_begin_short_backtrace(main)` passes a
// `main: fn() -> T` where the callee wants `F: FnOnce() -> T`; `T`
// is reachable only through the bound.
#[lang = "fn_once"]
trait FnOnce<Args> {
    type Output;
    fn call_once(self, args: Args) -> Self::Output;
}

fn run<T, F: FnOnce() -> T>(f: F) -> T {
    let v = f();
    v
}

fn three() -> u32 {
    3
}

fn call_ptr(main: fn() -> u32) -> u32 {
    let _ = run(main);
    run(main) + 1
}

#[no_mangle]
pub extern "C" fn fnptr_bound(x: u32) -> u32 {
    let _ = run(three);
    call_ptr(three) + x
}
