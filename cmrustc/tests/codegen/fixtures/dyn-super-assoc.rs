#![no_core]
#![feature(no_core)]
trait Base {
    type Out;
    fn produce(&self, x: u32) -> Self::Out;
}
trait Sub: Base {
    fn twice(&self, x: u32) -> u32;
}
struct Adder(u32);
impl Base for Adder {
    type Out = u32;
    fn produce(&self, x: u32) -> u32 {
        x + self.0
    }
}
impl Sub for Adder {
    fn twice(&self, x: u32) -> u32 {
        x + x
    }
}
fn drive(f: &mut dyn Sub<Out = u32>, x: u32) -> u32 {
    f.produce(x) + f.twice(x)
}
#[no_mangle]
pub fn probe_dyn_super_assoc(a: u32) -> u32 {
    let mut adder = Adder(2);
    drive(&mut adder, a)
}
