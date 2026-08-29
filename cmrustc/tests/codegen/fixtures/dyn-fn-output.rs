#![feature(no_core)]
#![no_core]
#![no_main]

// std's futex `Once::call(&self, _, f: &mut dyn FnMut(&OnceState))`
// calls `f(&state)`: the call's type is `<dyn FnMut<(A,)> as
// FnOnce>::Output`, which the object's `Output = R` binding resolves.
#[lang = "sized"]
trait Sized {}
#[lang = "fn_once"]
trait FnOnce<Args> {
    type Output;
    fn call_once(self, args: Args) -> Self::Output;
}
#[lang = "fn_mut"]
trait FnMut<Args>: FnOnce<Args> {
    fn call_mut(&mut self, args: Args) -> Self::Output;
}

fn run(f: &mut dyn FnMut(u32) -> u32, x: u32) -> u32 {
    let y = f(x);
    y + f(y)
}

#[no_mangle]
pub extern "C" fn dyn_fn_output(x: u32) -> u32 {
    let mut total = 0;
    let r = run(&mut |v| { total = total + v; v * 2 }, x);
    r + total
}
