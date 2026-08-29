#![no_core]
#![feature(no_core)]
macro cfg_unordered(
    $(#[cfg($cfg:meta)] $os:item)*
    #[else] $fallback:item
) {
    $(#[cfg($cfg)] $os)*
    #[cfg(not(any($($cfg),*)))] $fallback
}
cfg_unordered! {
#[cfg(windows)]
pub mod os {
    pub const CODE: u32 = 1;
}
#[cfg(unix)]
pub mod os {
    pub const CODE: u32 = 42;
}
#[else]
pub mod os {
    pub const CODE: u32 = 3;
}
}
#[no_mangle]
pub fn probe_decl_macro_brace() -> u32 { os::CODE }
