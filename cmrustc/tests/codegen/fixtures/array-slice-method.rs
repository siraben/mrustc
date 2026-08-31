#![feature(no_core)]
#![no_core]
#![no_main]

#[rustc_intrinsic]
pub unsafe fn ptr_metadata<P: ?Sized>(ptr: *const P) -> usize;

impl<T> [T] {
    pub fn len(&self) -> usize {
        unsafe { ptr_metadata(self) }
    }

    pub fn as_ptr(&self) -> *const T {
        self as *const [T] as *const T
    }
}

#[no_mangle]
pub extern "C" fn array_slice_method() -> usize {
    let bytes = b"llvm-bytes";
    unsafe { *bytes.as_ptr() as usize + bytes.len() }
}
