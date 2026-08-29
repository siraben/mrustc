#![feature(no_core)]
#![no_core]
#![no_main]

// std's unix `init`: a `mod sigpipe { pub const .. }` declared inside a
// function body, named `sigpipe::DEFAULT` in a match and as a value.
fn classify(x: u8) -> u8 {
    mod sigpipe {
        pub const DEFAULT: u8 = 0;
        pub const INHERIT: u8 = 1;
        pub fn twice(v: u8) -> u8 { v * 2 }
    }
    match x {
        sigpipe::DEFAULT => 10,
        sigpipe::INHERIT => sigpipe::twice(x) + 20,
        _ => sigpipe::twice(sigpipe::INHERIT) + 30,
    }
}

#[no_mangle]
pub extern "C" fn local_mod(x: u8) -> u8 {
    classify(x)
}
