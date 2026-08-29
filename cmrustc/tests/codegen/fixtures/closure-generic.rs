#![feature(no_core)]
#![no_core]
#![no_main]

// A closure inside a generic fn that calls a closure-typed parameter
// (core's `DebugTuple::field_with(value_fmt)`: `self.result.and_then(|_|
// value_fmt(self.fmt))`) — each instance of the enclosing fn needs its own
// copy of the closure body, since `F` differs per instance.
trait FnOnce<Args> {
    type Output;
    fn call_once(self, args: Args) -> Self::Output;
}

struct Holder {
    seed: u32,
}

impl Holder {
    fn apply<F: FnOnce(u32) -> u32>(&mut self, f: F) -> u32 {
        let run = |x: u32| f(x + self.seed);
        run(1)
    }
}

fn twice<F: FnOnce(u32) -> u32>(f: F, v: u32) -> u32 {
    let inner = |q: u32| f(q) * 2;
    inner(v)
}

#[no_mangle]
pub extern "C" fn closure_generic(k: u32) -> u32 {
    let mut h = Holder { seed: k };
    let a = h.apply(|v| v * 10);
    let b = h.apply(|v| v + 1000);
    let c = twice(|q| q + 5, k);
    let d = twice(|q| q * 100, k);
    a + b + c * 10000 + d * 1000000
}
