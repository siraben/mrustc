#![feature(no_core)]
#![no_core]
#![no_main]

fn identity<T>(x: T) -> T {
    x
}

fn caller<T>(x: T) -> T {
    identity::<T>(x)
}

#[no_mangle]
pub extern "C" fn probe_generic_chain(x: u32) -> u32 {
    caller::<u32>(x)
}
