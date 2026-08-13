#![feature(no_core)]
#![no_core]
#![no_main]

mod hidden {
    pub trait Value {
        fn value(self, other: u32) -> u32;
    }

    impl Value for u32 {
        fn value(self, other: u32) -> u32 {
            other
        }
    }
}

#[no_mangle]
pub extern "C" fn dot_value(input: u32, other: u32) -> u32 {
    input.value(other)
}
