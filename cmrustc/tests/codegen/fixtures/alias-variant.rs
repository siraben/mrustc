#![feature(no_core)]
#![no_core]
#![no_main]

// `Enum::Variant` spelled through a type alias (or an import of a
// dependency's enum: `Ordering::SeqCst`) is the variant value, not an
// item path.
enum Mode {
    Off,
    Low,
    High,
}

type M = Mode;

fn weight(m: Mode) -> u32 {
    match m {
        Mode::Off => 1,
        Mode::Low => 10,
        Mode::High => 100,
    }
}

#[no_mangle]
pub extern "C" fn alias_variant(x: u32) -> u32 {
    let a = M::Low;
    let b = if x > 5 { M::High } else { M::Off };
    weight(a) + weight(b) * 1000
}
