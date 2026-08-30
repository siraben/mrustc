#![feature(no_core)]
#![no_core]
#![no_main]

trait PartialEq<Rhs = Self> {
    fn eq(&self, other: &Rhs) -> bool;
}

struct Left;

/* Keep the array RHS first: receiver-only lookup used to select it for a
 * slice RHS as well, leaving N unconstrained and calling the wrong body. */
impl<U, const N: usize> PartialEq<&[U; N]> for Left {
    fn eq(&self, _other: &&[U; N]) -> bool {
        false
    }
}

impl<U> PartialEq<&[U]> for Left {
    fn eq(&self, _other: &&[U]) -> bool {
        true
    }
}

#[no_mangle]
pub extern "C" fn overlapping_slice_eq_probe() -> u32 {
    let right_array = [3u8, 4u8];
    let right: &[u8] = &right_array;
    if Left == right { 41 } else { 0 }
}
