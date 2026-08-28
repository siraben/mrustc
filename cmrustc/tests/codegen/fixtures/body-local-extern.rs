#![feature(no_core)]
#![no_core]
#![no_main]

// core's panic_fmt names its panic handler through an extern block
// declared inside the function body.
struct Info {
    code: u32,
}

fn check(info: &Info) -> u32 {
    unsafe extern "Rust" {
        fn host_abort(info: &Info) -> !;
    }
    if info.code > 1000 {
        unsafe { host_abort(info) }
    }
    info.code + 2
}

#[no_mangle]
pub extern "C" fn body_local_extern(code: u32) -> u32 {
    let info = Info { code: code };
    check(&info)
}
