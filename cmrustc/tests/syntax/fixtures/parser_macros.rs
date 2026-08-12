#![no_core]

macro_rules! make {
    ($name:ident) => { fn $name() {} };
}

pub macro assert_matches {
    ($expression:expr, $pattern:pat) => { () };
}

pub(crate) macro cfg_value($($cfg:tt)*) {
    false
}

crate::make!(generated);
braced! { outer([one, { two(three) }]) }
bracketed![a, (b), { c }];

const LINE: u32 = line!();

fn demo() {
    let text = concat!("a", nested!(["b"]));
    assert!(text);
    empty!();
}
