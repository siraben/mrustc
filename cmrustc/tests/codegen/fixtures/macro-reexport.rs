#![no_core]
#![feature(no_core)]
mod native {
    pub(crate) macro local_pointer {
        ($n:ident) => { pub fn $n() -> u32 { 5 } },
    }
}
mod tl {
    pub(crate) use crate::native::local_pointer;
}
mod user {
    use crate::tl::local_pointer;
    local_pointer!(five);
    pub fn go() -> u32 { five() }
}
#[no_mangle]
pub fn probe_macro_reexport() -> u32 { user::go() }
