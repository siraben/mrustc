#![feature(no_core)]
#![no_core]
#![no_main]

// std's fs/unix.rs `try_statx`: `enum STATX_STATE{ Unknown = 0, Present, Unavailable }`
// declared inside a function body and used as `STATX_STATE::Present as u8`.
fn state(x: u8) -> u8 {
    enum State { Unknown = 0, Present, Unavailable = 9 }
    if x == 0 { State::Unknown as u8 } else if x == 1 { State::Present as u8 + 10 } else { State::Unavailable as u8 + 20 }
}

#[no_mangle]
pub extern "C" fn local_enum(x: u8) -> u8 {
    state(x)
}
