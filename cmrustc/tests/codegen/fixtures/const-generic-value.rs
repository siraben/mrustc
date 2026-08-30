#![feature(no_core)]
#![no_core]
#![no_main]

fn has_len<const N: usize>(actual: usize) -> bool {
    actual == N
}

#[no_mangle]
pub extern "C" fn const_generic_value_probe() -> u32 {
    if has_len::<3>(3) && !has_len::<4>(3) {
        34
    } else {
        0
    }
}
