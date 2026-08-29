#![feature(no_core)]
#![no_core]
#![no_main]

// hashbrown's `fn extend_one(&mut self, &(k, v): &'a (K, V))`: destructuring
// parameter patterns — a reference around a tuple, and a bare tuple.
fn sum_ref(&(a, b): &(u32, u32)) -> u32 { a + b }

fn sum_tuple((a, b): (u32, u32)) -> u32 { a * 10 + b }

#[no_mangle]
pub extern "C" fn param_destructure(x: u32, y: u32) -> u32 {
    let pair = (x, y);
    sum_ref(&pair) + sum_tuple(pair)
}
