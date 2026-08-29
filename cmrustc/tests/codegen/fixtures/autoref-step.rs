#![feature(no_core)]
#![no_core]
#![no_main]

// A method found on the reference type itself (autoderef step 0) takes
// the address of the reference — `r.show()` on `r: &Item` calls
// `<&Item as Show>::show(&r)` (alloc's `impl SpecToString for &str` on a
// `&str`); one step further in, the reference is the address.
struct Item {
    v: u32,
}

trait Show {
    fn show(&self) -> u32;
}

impl Show for &Item {
    fn show(&self) -> u32 {
        (**self).v + 1
    }
}

trait Take {
    fn take(self) -> u32;
}

impl Take for &Item {
    fn take(self) -> u32 {
        self.v + 2
    }
}

#[no_mangle]
pub extern "C" fn autoref_step(x: u32) -> u32 {
    let it = Item { v: x };
    let r: &Item = &it;
    let rr: &&Item = &r;
    let a = r.show();
    let b = rr.show();
    let c = r.take();
    let d = rr.take();
    a + b * 100 + (c + d) * 10000
}
