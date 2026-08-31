#![feature(no_core)]
#![no_core]
#![no_main]

trait Clone {
    fn clone(&self) -> Self;
}

struct Range {
    start: u32,
    end: u32,
}

impl Clone for Range {
    fn clone(&self) -> Self {
        Range { start: self.start, end: self.end }
    }
}

impl<T> Clone for &T {
    fn clone(&self) -> Self {
        *self
    }
}

fn clone_range(range: &Range) -> Range {
    range.clone()
}

#[no_mangle]
pub extern "C" fn clone_reference_pointee(start: u32, end: u32) -> u32 {
    let range = Range { start, end };
    let copied = clone_range(&range);
    copied.start * 100 + copied.end
}
