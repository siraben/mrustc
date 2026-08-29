#![feature(no_core)]
#![no_core]
#![no_main]

// vec!'s `<[_]>::into_vec(box_new([x, 2]))`: the `Box<[T; N]>` passed
// where a `Box<[T]>` is expected unsizes like `&[T; N] -> &[T]` -- the
// box is a pointer to its contents, so it becomes the [block, N] pair.
#[lang = "deref"]
trait Deref {
    type Target;
    fn deref(&self) -> &Self::Target;
}

#[lang = "owned_box"]
struct Box<T: ?Sized>(*mut T);

#[rustc_intrinsic]
fn box_new<T>(x: T) -> Box<T>;

#[rustc_intrinsic]
pub unsafe fn ptr_metadata<P: ?Sized>(ptr: *const P) -> usize;

impl<T> [T] {
    pub fn len(&self) -> usize {
        unsafe { ptr_metadata(self) }
    }
}

impl<T: ?Sized> Deref for Box<T> {
    type Target = T;
    fn deref(&self) -> &T { &**self }
}

fn count(b: Box<[u32]>) -> u32 {
    let s: &[u32] = &*b;
    s.len() as u32 + s[1]
}

#[no_mangle]
pub extern "C" fn box_unsize(x: u32) -> u32 {
    count(box_new([x, 7, 9]))
}
