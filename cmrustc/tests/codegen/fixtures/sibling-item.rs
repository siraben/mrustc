#![feature(no_core)]
#![no_core]
#![no_main]

// std's `panicking::try`: `union Data<F, R>` and `fn do_call<F, R>` are
// both items inside `try`'s body; `try` builds the union and passes it as
// `*mut u8`, and `do_call` casts back to `*mut Data<F, R>` -- a body-local
// item named from a sibling body-local fn.
#[lang = "sized"]
trait Sized {}

struct Pair<T> {
    a: T,
    b: u32,
}

fn run<T>(value: T) -> u32 {
    union Data<T> {
        f: Pair<T>,
        n: u32,
    }
    fn do_call<T>(data: *mut u8) -> u32 {
        unsafe {
            let data = data as *mut Data<T>;
            let data = &mut (*data);
            data.f.b
        }
    }
    let mut data = Data { f: Pair { a: value, b: 37 } };
    do_call::<T>(&mut data as *mut Data<T> as *mut u8) + 5
}

#[no_mangle]
pub extern "C" fn sibling_item(x: u32) -> u32 {
    run::<u64>(7u64) + x
}
