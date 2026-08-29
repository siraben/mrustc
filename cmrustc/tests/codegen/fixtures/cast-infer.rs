#![feature(no_core)]
#![no_core]
#![no_main]

// `x as *mut _ as *mut T`: the inferred pointee of the first cast comes
// from the reference being cast (core's `MaybeUninit::as_mut_ptr`).
struct Cell<T> {
    value: T,
}

impl<T> Cell<T> {
    fn as_mut_ptr(&mut self) -> *mut T {
        self as *mut _ as *mut T
    }
    fn as_ptr(&self) -> *const T {
        self as *const _ as *const T
    }
}

#[no_mangle]
pub extern "C" fn cast_infer(x: u32) -> u32 {
    let mut c = Cell { value: x };
    let p = c.as_mut_ptr();
    unsafe { *p = *p + 5; }
    let q = c.as_ptr();
    unsafe { *q + c.value }
}
