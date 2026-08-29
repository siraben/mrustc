#![feature(no_core)]
#![no_core]
#![no_main]

// std's `impl Write for Stdout { fn write_fmt(&mut self, args) {
// (&*self).write_fmt(args) } }` dispatches to `impl Write for &Stdout`:
// on a `&Out` receiver the by-value step (autoref `&mut &Out`) matches
// the reference impl before any deref step reaches `Out`'s own impl.
trait Write {
    fn write_fmt(&mut self, x: u32) -> u32;
}

struct Out {
    base: u32,
}

impl Write for Out {
    fn write_fmt(&mut self, x: u32) -> u32 { (&*self).write_fmt(x) }
}

impl Write for &Out {
    fn write_fmt(&mut self, x: u32) -> u32 { self.base + x }
}

#[no_mangle]
pub extern "C" fn ref_impl_pick(x: u32) -> u32 {
    let mut o = Out { base: 100 };
    o.write_fmt(x)
}
