#![feature(no_core)]
#![no_core]
#![no_main]

#[lang = "sized"]
trait Sized {}

struct Num(u32);

impl Num {
    fn get(&self) -> u32 { self.0 }
}

enum Bound<T> {
    Included(T),
    Excluded(T),
}

trait Bounds<T> {
    fn start(&self) -> Bound<&T>;
}

struct Holder<'a>(&'a Num, bool);

impl Bounds<Num> for Holder<'_> {
    fn start(&self) -> Bound<&Num> {
        if self.1 { Bound::Included(self.0) }
        else { Bound::Excluded(self.0) }
    }
}

fn read<R: Bounds<Num>>(range: &R) -> u32 {
    match range.start() {
        Bound::Included(value) => value.get(),
        Bound::Excluded(value) => value.get() + 1,
    }
}

#[no_mangle]
pub extern "C" fn pattern_generic(x: u32) -> u32 {
    let n = Num(x);
    read(&Holder(&n, true)) + read(&Holder(&n, false))
}
