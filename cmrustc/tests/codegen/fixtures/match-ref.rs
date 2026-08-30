#![feature(no_core)]
#![no_core]
#![no_main]

// `match self` in a `&self` method (default binding modes): the
// discriminant is read through the reference, as the payload slots are.
#[lang = "sized"]
trait Sized {}

enum Opt<T> {
    Some(T),
    None,
}

impl Opt<u32> {
    fn peek(&self) -> u32 {
        match self {
            Opt::Some(v) => *v,
            Opt::None => 0,
        }
    }
}

impl<T> Opt<T> {
    fn is_some(&self) -> u32 {
        match self {
            Opt::Some(_) => 1,
            Opt::None => 0,
        }
    }
}

#[no_mangle]
pub extern "C" fn match_ref(x: u32) -> u32 {
    let o: Opt<u32> = Opt::Some(7);
    let n: Opt<u32> = Opt::None;
    o.peek() + o.is_some() + n.peek() + n.is_some() + x
}
