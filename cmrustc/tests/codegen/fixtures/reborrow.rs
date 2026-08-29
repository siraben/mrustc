#![feature(no_core)]
#![no_core]
#![no_main]

// `&*p` is `p` itself: a reborrowed reference returned from a function
// must not point into that function's frame.
fn same(p: &u32) -> &u32 {
    &*p
}

fn through(p: *const u32) -> &'static u32 {
    unsafe { &*p }
}

fn clobber(n: u32) -> u32 {
    let a = n + 1;
    let b = a + 1;
    let c = b + 1;
    a + b + c
}

#[no_mangle]
pub extern "C" fn reborrow(v: u32) -> u32 {
    let x = v;
    let r = same(&x);
    let q = through(&x as *const u32);
    let noise = clobber(v);
    *r + *q + noise
}
