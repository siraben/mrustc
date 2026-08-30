#![feature(no_core)]
#![no_core]
#![no_main]

trait Deref {
    type Target;
    fn deref(&self) -> &Self::Target;
}

struct Leaf(u32);

impl Leaf {
    fn value(&self) -> u32 {
        self.0
    }
}

struct Middle(Leaf);

impl Deref for Middle {
    type Target = Leaf;

    fn deref(&self) -> &Leaf {
        &self.0
    }
}

struct Outer(Middle);

impl Deref for Outer {
    type Target = Middle;

    fn deref(&self) -> &Middle {
        &self.0
    }
}

#[no_mangle]
pub extern "C" fn multi_deref_method_probe() -> u32 {
    let outer = Outer(Middle(Leaf(73)));
    outer.value()
}
