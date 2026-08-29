#![feature(no_core)]
#![no_core]
#![no_main]

// `#[cfg]` on struct-literal fields selects one initializer.
struct Pair {
    a: u32,
    b: u32,
}

fn make(x: u32) -> Pair {
    Pair {
        a: x,
        #[cfg(not(any(sanitize = "cfi", sanitize = "kcfi")))]
        b: x + 1,
        #[cfg(any(sanitize = "cfi", sanitize = "kcfi"))]
        b: 1000,
    }
}

#[no_mangle]
pub extern "C" fn cfg_field(x: u32) -> u32 {
    let p = make(x);
    p.a * 10 + p.b
}
