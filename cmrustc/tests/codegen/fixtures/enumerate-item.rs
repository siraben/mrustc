#![feature(no_core)]
#![no_core]
#![no_main]

// A loop binding typed through an associated-type projection
// (`<Iter as Iterator>::Item = &T`) is a reference: no autoref.
enum Option<T> { None, Some(T) }
trait Iterator {
    type Item;
    fn next(&mut self) -> Option<Self::Item>;
}
struct It<'a, T> { p: &'a T, left: u32 }
impl<'a, T> Iterator for It<'a, T> {
    type Item = &'a T;
    fn next(&mut self) -> Option<&'a T> {
        if self.left == 0 { Option::None } else { self.left = self.left - 1; Option::Some(self.p) }
    }
}
struct Enumerate<I> { it: I, c: usize }
impl<I: Iterator> Iterator for Enumerate<I> {
    type Item = (usize, I::Item);
    fn next(&mut self) -> Option<(usize, I::Item)> {
        match self.it.next() {
            Option::Some(v) => { let i = self.c; self.c = self.c + 1; Option::Some((i, v)) }
            Option::None => Option::None,
        }
    }
}
struct Arg { v: u32 }
impl Arg { fn fmt(&self) -> u32 { self.v } }
#[no_mangle]
pub extern "C" fn enumerate(v: u32) -> u32 {
    let a = Arg { v: v };
    let mut s = 0;
    let en = Enumerate { it: It { p: &a, left: 2 }, c: 0 };
    for (i, arg) in en {
        s = s + arg.fmt() + i as u32;
    }
    s
}
