#![feature(no_core)]
#![no_core]
#![no_main]

// Iterator adapters describe closure arguments with nested associated-type
// projections (Zip::Item is a tuple of its inputs' Item projections).  All
// tuple layers must be normalized before checking a destructuring closure.
trait FnOnce<Args> {
    type Output;
    fn call_once(self, args: Args) -> Self::Output;
}

trait Source {
    type Item;
    fn item(self) -> Self::Item;
}

trait Apply {
    type Item;
    fn apply<F: FnOnce(Self::Item) -> u32>(self, f: F) -> u32;
}

struct Left(u32, u32);
struct Right(u32, u32);
struct Zip<A, B> {
    left: A,
    right: B,
}

impl Source for Left {
    type Item = (u32, u32);
    fn item(self) -> Self::Item { (self.0, self.1) }
}

impl Source for Right {
    type Item = (u32, u32);
    fn item(self) -> Self::Item { (self.0, self.1) }
}

impl<A: Source, B: Source> Apply for Zip<A, B> {
    type Item = (A::Item, B::Item);

    fn apply<F: FnOnce(Self::Item) -> u32>(self, f: F) -> u32 {
        f((self.left.item(), self.right.item()))
    }
}

#[no_mangle]
pub extern "C" fn associated_item_closure_param(k: u32) -> u32 {
    Zip { left: Left(k, 2), right: Right(3, 4) }
        .apply(|((a, b), (c, d))| a * 1000 + b * 100 + c * 10 + d)
}
