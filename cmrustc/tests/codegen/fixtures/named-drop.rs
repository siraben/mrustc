#![feature(no_core)]
#![no_core]
#![no_main]

#[lang = "sized"]
trait Sized {}
#[lang = "drop"]
trait Drop {
    fn drop(&mut self);
}

static mut DROPS: u32 = 0;

struct Guard;

impl Drop for Guard {
    fn drop(&mut self) {
        unsafe { DROPS = DROPS + 1; }
    }
}

fn one_scope() {
    let _guard = Guard;
}

fn return_scope() {
    let _guard = Guard;
    return;
}

#[no_mangle]
pub extern "C" fn named_drop() -> u32 {
    one_scope();
    one_scope();
    return_scope();
    unsafe { DROPS }
}
