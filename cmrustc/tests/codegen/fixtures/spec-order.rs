#![feature(no_core)]
#![feature(specialization)]
#![no_core]
#![no_main]

mod util {
    pub struct Empty;
    impl crate::SizeHint for Empty {
        fn upper_bound(&self) -> u32 { 0 }
    }
}

trait SizeHint {
    fn lower_bound(&self) -> u32;
    fn upper_bound(&self) -> u32;
}

impl<T: ?Sized> SizeHint for T {
    default fn lower_bound(&self) -> u32 { 1 }
    default fn upper_bound(&self) -> u32 { 2 }
}

#[no_mangle]
pub extern "C" fn spec_order() -> u32 {
    let e = util::Empty;
    e.lower_bound() * 10 + e.upper_bound()
}
