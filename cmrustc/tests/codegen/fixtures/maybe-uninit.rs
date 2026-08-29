#![feature(no_core)]
#![no_core]
#![no_main]

// A wrapper whose only non-zero-sized field is T (MaybeUninit<T>,
// ManuallyDrop<T>) is represented as T, including its scalar width, so
// a [MaybeUninit<u8>; N] buffer reads back as bytes.
struct MD<T> {
    value: T,
}

union MU<T> {
    uninit: (),
    value: MD<T>,
}

#[rustc_intrinsic]
pub unsafe fn offset<Ptr, Delta>(dst: Ptr, offset: Delta) -> Ptr;

#[no_mangle]
pub extern "C" fn maybe_uninit(x: u8) -> u32 {
    let mut buf: [MU<u8>; 4] = [MU { uninit: () }; 4];
    buf[1] = MU { value: MD { value: x } };
    buf[2] = MU { value: MD { value: 9 } };
    let p = &buf as *const [MU<u8>; 4] as *const u8;
    let a = unsafe { *offset(p, 1) } as u32;
    let b = unsafe { *offset(p, 2) } as u32;
    let m = MD { value: 5u32 };
    a * 100 + b * 10 + m.value
}
