#![feature(no_core)]
#![no_core]
#![no_main]

// thread_local!'s `LocalKey::new(|init| ..)`: a capture-free closure
// where a `fn` pointer is expected coerces to one -- the pointer is a
// thunk taking the closure's parameters (a closure body's own C shape
// takes its environment first).
fn apply(f: fn(u32) -> u32, x: u32) -> u32 {
    f(x)
}

fn pick(flag: bool) -> fn(u32) -> u32 {
    if flag { |v| v + 1 } else { |v| v * 2 }
}

#[no_mangle]
pub extern "C" fn closure_fnptr(x: u32) -> u32 {
    let g: fn(u32) -> u32 = |v| v + 10;
    apply(|v| v * 3, x) + g(x) + pick(true)(x) + pick(false)(x)
}
