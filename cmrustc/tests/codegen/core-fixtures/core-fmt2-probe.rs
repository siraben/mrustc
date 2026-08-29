#![no_std]
#![no_main]

use core::fmt::Write;

struct Buffer {
    bytes: [u8; 64],
    len: usize,
}

impl Write for Buffer {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for b in s.bytes() {
            if self.len < 64 {
                self.bytes[self.len] = b;
                self.len += 1;
            }
        }
        Ok(())
    }
}

/// Formats `value` several ways into `out` (up to `cap` bytes); returns
/// the number of bytes written.
#[no_mangle]
pub extern "C" fn probe_fmt2(which: u32, value: u32, out: *mut u8, cap: usize) -> usize {
    let mut buffer = Buffer { bytes: [0u8; 64], len: 0 };
    let name = "x";
    let _ = match which {
        0 => write!(buffer, "{}={}", name, value),
        1 => write!(buffer, "{:x}", value),
        2 => write!(buffer, "[{:>5}]", value),
        3 => write!(buffer, "{}", value as i32 - 10),
        4 => write!(buffer, "{:?}", value > 3),
        5 => write!(buffer, "{:?}", if value > 3 { Some(value) } else { None }),
        _ => write!(buffer, "{:08b}", value),
    };
    let mut i = 0;
    while i < buffer.len && i < cap {
        unsafe { *out.add(i) = buffer.bytes[i]; }
        i += 1;
    }
    buffer.len
}
