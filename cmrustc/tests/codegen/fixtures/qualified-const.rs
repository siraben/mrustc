#![feature(no_core)]
#![no_core]
#![no_main]

// `<Ty>::CONST` (a qualified path with no trait) reaches an inherent or
// trait associated const — core's `isize::MAX = (<usize>::MAX >> 1) as
// Self` reads `<usize>::MAX`.
trait Bound {
    const LIMIT: u32;
}

struct Small;
struct Large;

impl Small {
    const BASE: u32 = 10;
}

impl Bound for Small {
    const LIMIT: u32 = 100;
}

impl Bound for Large {
    const LIMIT: u32 = <Small>::LIMIT * 2 + <Small>::BASE;
}

fn clamp<T: Bound>(x: u32) -> u32 {
    if x > <T>::LIMIT { <T>::LIMIT } else { x }
}

#[no_mangle]
pub extern "C" fn qualified_const(x: u32) -> u32 {
    let shifted = (<Small>::LIMIT >> 1) as u32;
    clamp::<Small>(x) + clamp::<Large>(x) * 1000 + shifted
}
