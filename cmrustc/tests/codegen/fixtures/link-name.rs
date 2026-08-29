#![feature(no_core)]
#![no_core]
#![no_main]

// std's `errno_location` is `#[cfg_attr(target_os = "linux", link_name =
// "__errno_location")]`: a foreign declaration binds to its link name,
// not its Rust name.
extern "C" {
    #[cfg_attr(target_os = "linux", link_name = "cm_host_errno_slot")]
    fn errno_slot() -> *mut u32;
    #[link_name = "cm_host_base"]
    static BASE: u32;
}

#[no_mangle]
pub extern "C" fn link_name(x: u32) -> u32 {
    unsafe {
        *errno_slot() = x;
        *errno_slot() + BASE
    }
}
