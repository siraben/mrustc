#![feature(no_core)]
#![no_core]
#![no_main]

#[lang = "sized"]
trait Sized {}

trait ChangePointee<P: ?Sized> {
    type Pointee: ?Sized;
    type Output: ?Sized;
}

impl<T, U: ?Sized> ChangePointee<U> for *mut T {
    type Pointee = T;
    type Output = *mut U;
}

#[rustc_intrinsic]
unsafe fn slice_get_unchecked<
    ItemPtr: ChangePointee<[T], Pointee = T, Output = SlicePtr>,
    SlicePtr,
    T,
>(slice_ptr: SlicePtr, index: usize) -> ItemPtr;

impl<T> [T] {
    unsafe fn put(&mut self, index: usize, value: T) {
        *slice_get_unchecked::<*mut T, *mut [T], T>(
            self as *mut [T], index,
        ) = value;
    }
}

#[no_mangle]
pub extern "C" fn slice_get_unchecked_width() -> u32 {
    let mut bytes = [1_u8, 2, 3];
    unsafe {
        bytes.put(1, 9);
    }

    let mut words = [10_u32, 20, 30];
    unsafe {
        words.put(2, 40);
    }

    bytes[1] as u32 + words[2]
}
