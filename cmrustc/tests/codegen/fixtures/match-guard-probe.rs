#![feature(no_core)]
#![no_core]
#![no_main]

enum Kind {
    Small(u32),
    Big(u32),
    Other,
}

fn classify(k: Kind, limit: u32) -> u32 {
    match k {
        Kind::Small(v) if v < limit => v,
        Kind::Small(v) => v + 100u32,
        Kind::Big(v) | Kind::Other => 7u32,
    }
}

#[no_mangle]
pub extern "C" fn probe_guard(value: u32, limit: u32) -> u32 {
    classify(Kind::Small(value), limit)
}

#[no_mangle]
pub extern "C" fn probe_or(value: u32) -> u32 {
    classify(Kind::Big(value), 0u32) + classify(Kind::Other, 0u32)
}
