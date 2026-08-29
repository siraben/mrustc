#![feature(no_core)]
#![no_core]
#![no_main]

// A `&mut dyn Write` built from a `&mut &mut Buffer` dispatches to the
// forwarding `impl Write for &mut W`, whose data pointer is the outer
// reference (core's `fmt::write(&mut self, args)` with `self: &mut W`).
trait Write {
    fn write_byte(&mut self, b: u8);
}

trait Sized {}
trait Copy {}

struct Buffer {
    bytes: [u8; 8],
    len: usize,
}

impl Write for Buffer {
    fn write_byte(&mut self, b: u8) {
        if self.len < 8 {
            self.bytes[self.len] = b;
            self.len += 1;
        }
    }
}

impl<W: Write + ?Sized> Write for &mut W {
    fn write_byte(&mut self, b: u8) {
        (**self).write_byte(b)
    }
}

fn emit(out: &mut dyn Write, b: u8) {
    out.write_byte(b);
    out.write_byte(b + 1);
}

fn through<W: Write + ?Sized>(mut w: &mut W, b: u8) {
    emit(&mut w, b);
}

#[no_mangle]
pub extern "C" fn dyn_forward(b: u32) -> u32 {
    let mut buffer = Buffer { bytes: [0u8; 8], len: 0 };
    through(&mut buffer, b as u8);
    buffer.len as u32 * 100 + buffer.bytes[0] as u32 + buffer.bytes[1] as u32
}
