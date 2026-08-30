#![feature(no_core)]
#![no_core]
#![no_main]

#[no_mangle]
pub extern "C" fn str_content_eq_probe() -> u32 {
    let first = "same";
    let second = "same";
    let different = "other";
    if first == second {
        if first != different { 47 } else { 0 }
    } else {
        0
    }
}
