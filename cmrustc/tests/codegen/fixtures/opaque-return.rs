#![feature(no_core)]
#![no_core]
#![no_main]

#[lang = "sized"]
trait Sized {}

// std's `panic_output() -> Option<impl io::Write>`: the hidden type
// comes from the callee body, typed on demand when the caller is
// checked first.
trait Tag {
    fn tag(&self) -> u32;
}

struct Two;

impl Tag for Two {
    fn tag(&self) -> u32 { 2 }
}

enum Opt<T> {
    Some(T),
    None,
}

#[no_mangle]
pub extern "C" fn opaque_return(x: u32) -> u32 {
    match maker(x > 5) {
        Opt::Some(t) => t.tag() + x,
        Opt::None => x,
    }
}

fn maker(flag: bool) -> Opt<impl Tag> {
    if flag { Opt::Some(Two) } else { Opt::None }
}
