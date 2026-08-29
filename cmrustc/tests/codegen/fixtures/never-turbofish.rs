#![feature(no_core, never_type)]
#![no_core]
#![no_main]

#[lang = "sized"]
trait Sized {}
#[lang = "fn_once"]
trait FnOnce<Args> {
    type Output;
    fn call_once(self, args: Args) -> Self::Output;
}

// core's `OnceLock::get_or_init`: `get_or_try_init(|| Ok::<T, !>(f()))` --
// the turbofish on a variant path binds the enum's generics; nothing else
// constrains `E`.
enum R<T, E> {
    Ok(T),
    Err(E),
}

use R::Ok;

fn get_or_try_init<T, E, F: FnOnce() -> R<T, E>>(f: F) -> R<T, E> {
    f()
}

fn get_or_init<T, F: FnOnce() -> T>(f: F) -> T {
    match get_or_try_init(|| Ok::<T, !>(f())) {
        Ok(v) => v,
        R::Err(never) => never,
    }
}

#[no_mangle]
pub extern "C" fn never_turbofish(x: u32) -> u32 {
    get_or_init(|| x + 1)
}
