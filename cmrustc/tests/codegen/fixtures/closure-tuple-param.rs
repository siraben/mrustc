#![feature(no_core)]
#![no_core]
#![no_main]

// A closure whose parameter is a destructuring pattern (core's
// `layout_array`: `.map(|(layout, _pad)| layout)`).
trait FnOnce<Args> {
    type Output;
    fn call_once(self, args: Args) -> Self::Output;
}

struct Pair {
    a: u32,
    b: u32,
}

fn apply<F: FnOnce((u32, u32)) -> u32>(f: F, t: (u32, u32)) -> u32 {
    f(t)
}

fn apply_pair<F: FnOnce(Pair) -> u32>(f: F, p: Pair) -> u32 {
    f(p)
}

#[no_mangle]
pub extern "C" fn closure_tuple_param(k: u32) -> u32 {
    let first = apply(|(x, _y)| x * 10, (k, 99));
    let second = apply(|(_x, y)| y + 1, (k, 5));
    let third = apply_pair(|Pair { a, b }| a * 100 + b, Pair { a: k, b: 7 });
    first + second * 1000 + third * 100000
}
