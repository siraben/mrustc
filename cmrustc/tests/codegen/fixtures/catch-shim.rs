#![feature(no_core)]
#![no_core]
#![no_main]

// std's `panicking::try`: `intrinsics::catch_unwind(do_call::<F, R>,
// data_ptr, do_catch::<F, R>)` -- without unwinding the try fn simply
// runs and the intrinsic returns 0.
#[lang = "sized"]
trait Sized {}

#[rustc_intrinsic]
unsafe fn catch_unwind(_try_fn: fn(*mut u8), _data: *mut u8, _catch_fn: fn(*mut u8, *mut u8)) -> i32;

fn bump(data: *mut u8) {
    unsafe {
        let p = data as *mut u32;
        *p = *p + 5;
    }
}

fn never(_data: *mut u8, _payload: *mut u8) {}

#[no_mangle]
pub extern "C" fn catch_shim(x: u32) -> u32 {
    let mut v = x;
    let rc = unsafe { catch_unwind(bump, &mut v as *mut u32 as *mut u8, never) };
    v + (rc as u32)
}
