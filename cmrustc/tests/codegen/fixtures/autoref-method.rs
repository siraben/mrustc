#![feature(no_core)]
#![no_core]
#![no_main]

struct Payload {
    value: u32,
}

trait Shared {
    fn value(&self, other: Payload) -> u32;
}

trait Mutable {
    fn value_mut(&mut self, other: Payload) -> u32;
}

impl Shared for u32 {
    fn value(&self, other: Payload) -> u32 {
        other.value
    }
}

impl Mutable for u32 {
    fn value_mut(&mut self, other: Payload) -> u32 {
        other.value
    }
}

fn bump(value: u32) -> u32 {
    value + 1
}

#[no_mangle]
pub extern "C" fn shared_autoref(input: u32, other: u32) -> u32 {
    input.value(Payload { value: bump(other) })
}

#[no_mangle]
pub extern "C" fn mutable_autoref(mut input: u32, other: u32) -> u32 {
    input.value_mut(Payload { value: bump(other) })
}
