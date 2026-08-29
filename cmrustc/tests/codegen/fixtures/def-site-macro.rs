#![feature(no_core)]
#![feature(decl_macro)]
#![no_core]
#![no_main]

// std's `sys/pal/unix/weak.rs`: `pub(crate) macro syscall { .. }` expands
// to `weak!(..)`, and `weak` is in scope only where `syscall` is defined;
// kernel_copy.rs imports just `syscall`. Names in an expansion resolve at
// the macro's definition module too.
mod w {
    pub(crate) macro weak {
        (fn $name:ident() -> $ret:ty;) => {
            fn $name() -> $ret { 7 }
        }
    }
    pub(crate) macro syscall {
        (fn $name:ident() -> $ret:ty;) => {
            weak!(fn $name() -> $ret;);
        }
    }
}

mod user {
    use crate::w::syscall;
    pub fn go() -> u32 {
        syscall!(fn seven() -> u32;);
        seven()
    }
}

#[no_mangle]
pub extern "C" fn def_site_macro() -> u32 {
    user::go()
}
