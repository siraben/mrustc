#![feature(no_core)]
#![no_core]
#![no_main]

#[cfg(all(
    unix,
    target_thread_local,
    target_arch = "x86_64",
    target_os = "linux",
    target_env = "gnu",
    target_abi = "",
    target_vendor = "unknown",
    target_family = "unix",
    target_pointer_width = "64",
    target_endian = "little",
    target_feature = "sse2",
    target_has_atomic = "64",
    target_has_atomic_equal_alignment = "64",
    target_has_atomic_load_store = "64",
))]
#[no_mangle]
pub extern "C" fn cfg_probe(_value: u32) -> u32 {
    64u32
}

#[cfg(all(
    unix,
    target_thread_local,
    target_arch = "x86",
    target_env = "musl",
    target_abi = "",
    target_pointer_width = "32",
    target_endian = "little",
    not(target_feature = "sse2"),
    target_has_atomic = "32",
    not(target_has_atomic = "64"),
    target_has_atomic_equal_alignment = "32",
    target_has_atomic_load_store = "32",
))]
#[no_mangle]
pub extern "C" fn cfg_probe(_value: u32) -> u32 {
    32u32
}
