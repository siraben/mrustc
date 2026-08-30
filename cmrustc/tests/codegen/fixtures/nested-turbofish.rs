#![feature(no_core)]
#![no_core]
#![no_main]

// std's `try`: `intrinsics::catch_unwind(do_call::<F, R>, data_ptr, ..)`
// names a body-local fn with a turbofish and takes it as a `fn(*mut u8)`
// pointer that mentions neither generic; the turbofish alone must bind
// them to the enclosing fn's parameters, else the instance is emitted
// over unresolved types and its calls vanish.
#[lang = "sized"]
trait Sized {}

trait Tag {
    fn tag(&self) -> u32;
}

struct Seven;

impl Tag for Seven {
    fn tag(&self) -> u32 { 7 }
}

fn run<T: Tag>(t: T) -> u32 {
    fn inner<T: Tag>(p: *mut u8) -> u32 {
        unsafe { (*(p as *mut T)).tag() + 1 }
    }
    let mut t = t;
    let f: fn(*mut u8) -> u32 = inner::<T>;
    f(&mut t as *mut T as *mut u8)
}

#[no_mangle]
pub extern "C" fn nested_turbofish(x: u32) -> u32 {
    run(Seven) + x
}
