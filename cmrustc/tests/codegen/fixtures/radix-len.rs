#![no_core]
#![feature(no_core)]
struct H { a: [u8; 0x051C], b: [u8; 0b101], c: [u8; 16usize], d: [u8; 0o17], e: [u8; 1_000] }
#[no_mangle]
pub fn probe_radix_len(x: u8) -> u32 { let h = H { a: [x; 0x051C], b: [x; 0b101], c: [x; 16usize], d: [x; 0o17], e: [x; 1_000] }; h.a[1307] as u32 + h.b[4] as u32 + h.c[15] as u32 + h.d[14] as u32 + h.e[999] as u32 }
