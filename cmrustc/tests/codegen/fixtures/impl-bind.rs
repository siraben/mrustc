#![feature(no_core)]
#![no_core]
#![no_main]

// An impl selected by parameter shape must bind its own `T` from the
// caller's *substituted* operand types: `tail<T>` passes its bare `T`
// slice to `<Skip as Index<[T]>>::at`, whose body offsets the element
// pointer by `T`'s width (core: `get_offset_len_noubcheck` under
// `<Range<usize> as SliceIndex<[MaybeUninit<u8>]>>`).
#[rustc_intrinsic]
pub unsafe fn offset<Ptr, Delta>(dst: Ptr, offset: Delta) -> Ptr;

trait Index<S: ?Sized> {
    fn at(self, s: *const S) -> *const u8;
}

struct Skip(usize);

impl<T> Index<[T]> for Skip {
    fn at(self, s: *const [T]) -> *const u8 {
        let p = s as *const T;
        unsafe { offset(p, self.0) as *const u8 }
    }
}

fn tail<T, I: Index<[T]>>(s: &[T], i: I) -> *const u8 {
    i.at(s as *const [T])
}

#[no_mangle]
pub extern "C" fn impl_bind(k: u32) -> u32 {
    let bytes: [u8; 6] = [10, 20, 30, 40, 50, 60];
    let p = tail(&bytes, Skip(k as usize));
    unsafe { *p as u32 }
}
