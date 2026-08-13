#![feature(no_core)]
#![no_core]
#![no_main]

trait FirstValue {
    fn value(self, other: u32) -> u32;
}

trait SecondValue {
    fn value(self, other: u32) -> u32;
}

impl FirstValue for u32 {
    fn value(self, other: u32) -> u32 {
        other
    }
}

impl SecondValue for u32 {
    fn value(self, other: u32) -> u32 {
        other
    }
}

#[no_mangle]
pub extern "C" fn dot_value(input: u32) -> u32 {
    input.value(input)
}
