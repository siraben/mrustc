#![feature(no_core)]
#![no_core]
#![no_main]

// Operators on non-scalar operands call their trait methods.
trait PartialEq<Rhs = Self> {
    fn eq(&self, other: &Rhs) -> bool;
    fn ne(&self, other: &Rhs) -> bool {
        !self.eq(other)
    }
}

trait Add<Rhs = Self> {
    type Output;
    fn add(self, rhs: Rhs) -> Self::Output;
}

struct P(u32, u32);

impl PartialEq for P {
    fn eq(&self, other: &P) -> bool {
        self.0 == other.0 && self.1 == other.1
    }
}

impl Add for P {
    type Output = P;
    fn add(self, rhs: P) -> P {
        P(self.0 + rhs.0, self.1 + rhs.1)
    }
}

#[no_mangle]
pub extern "C" fn op_traits(x: u32) -> u32 {
    let a = P(x, 7);
    let b = P(x, 7);
    let c = P(1, 2) + P(3, 4);
    let mut total = c.0 * 10 + c.1;
    if a == b { total = total + 100; }
    if a != c { total = total + 1000; }
    total
}
