#![feature(no_core)]
#![no_core]
#![no_main]

// A fn item passed where `F: FnOnce(u32) -> u32` is expected and called
// through the local (`Option::map_or_else(.., ToOwned::to_owned)`).
#[lang = "fn_once"]
trait FnOnce<Args> {
    type Output;
    fn call_once(self, args: Args) -> Self::Output;
}

fn double(x: u32) -> u32 {
    x * 2
}

fn add_three(x: u32) -> u32 {
    x + 3
}

fn apply<F: FnOnce(u32) -> u32>(f: F, x: u32) -> u32 {
    f(x)
}

fn pick<F: FnOnce(u32) -> u32, G: FnOnce(u32) -> u32>(flag: bool, f: F, g: G, x: u32) -> u32 {
    if flag { f(x) } else { g(x) }
}

#[no_mangle]
pub extern "C" fn fn_item_value(x: u32) -> u32 {
    apply(double, x) + pick(x > 5, double, add_three, x) * 100
}
