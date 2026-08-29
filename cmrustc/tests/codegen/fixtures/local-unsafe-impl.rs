#![no_core]
#![feature(no_core)]
#[no_mangle]
pub fn probe_local_unsafe_impl(a: u32) -> u32 {
    unsafe trait Tag {
        fn tag(&self) -> u32;
    }
    struct Holder {
        value: u32,
    }
    unsafe impl Tag for Holder {
        fn tag(&self) -> u32 {
            self.value + 1
        }
    }
    let h = Holder { value: a };
    h.tag()
}
