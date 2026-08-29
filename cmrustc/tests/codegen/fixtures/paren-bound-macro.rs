#![no_core]
#![feature(no_core)]
trait Produce { fn produce(&self) -> u32; }
trait Marker {}
struct Box7;
impl Produce for Box7 { fn produce(&self) -> u32 { 7 } }
impl Marker for Box7 {}
fn make() -> impl (Produce) { Box7 }
fn run(p: &(dyn Produce)) -> u32 { p.produce() }
#[no_mangle]
pub fn probe_paren_bound_macro(a: u32) -> u32 {
    macro bump($x:expr) { $x + 1 }
    let m = Box7;
    let d: &dyn Produce = &m;
    run(d) + bump!(a)
}
