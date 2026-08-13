#![feature(no_core)]
#![no_core]
#![no_main]

trait Echo {
    fn echo(self, value: Self) -> Self;
}

impl<T> Echo for T {
    fn echo(self, value: T) -> T {
        value
    }
}

#[no_mangle]
pub extern "C" fn echo_qualified_u32(receiver: u32, value: u32) -> u32 {
    <u32 as Echo>::echo(receiver, value)
}

#[no_mangle]
pub extern "C" fn echo_dot_u8(receiver: u8, value: u8) -> u8 {
    receiver.echo(value)
}
