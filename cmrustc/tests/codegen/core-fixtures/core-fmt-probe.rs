#![no_std]
#![no_main]

use core::fmt::Write;

struct Buffer {
    bytes: [u8; 32],
    len: usize,
}

impl Write for Buffer {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for b in s.bytes() {
            if self.len < 32 {
                self.bytes[self.len] = b;
                self.len += 1;
            }
        }
        Ok(())
    }
}

#[no_mangle]
pub extern "C" fn probe_fmt_len(value: u32) -> u32 {
    let mut buffer = Buffer { bytes: [0u8; 32], len: 0 };
    let _ = write!(buffer, "{}", value);
    buffer.len as u32
}
