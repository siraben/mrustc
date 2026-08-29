#![feature(no_core)]
#![no_core]
#![no_main]

// core's `OnceLock::get_or_try_init`: `self.initialize(f)?` -- the
// callee's own `E` is reachable only by matching its `F: FnOnce() ->
// Result<T, E>` bound against the caller's bound on the `F` it passes.
#[lang = "sized"]
trait Sized {}
#[lang = "fn_once"]
trait FnOnce<Args> {
    type Output;
    fn call_once(self, args: Args) -> Self::Output;
}

enum R<T, E> {
    Ok(T),
    Err(E),
}

struct Slot {
    seen: u32,
}

impl Slot {
    fn initialize<T, F: FnOnce() -> R<T, E>, E>(&mut self, f: F) -> R<(), E> {
        match f() {
            R::Ok(_) => { self.seen = self.seen + 1; R::Ok(()) }
            R::Err(e) => R::Err(e),
        }
    }

    fn get_or_try_init<T, F: FnOnce() -> R<T, E>, E>(&mut self, f: F) -> R<u32, E> {
        // The callee's result is dropped: its `E` is reachable only
        // through the bound on `F`.
        let _ = self.initialize(f);
        R::Ok(self.seen)
    }
}

#[no_mangle]
pub extern "C" fn bound_infer(x: u32) -> u32 {
    let mut slot = Slot { seen: 10 };
    match slot.get_or_try_init(|| if x > 100 { R::Err(7u32) } else { R::Ok(x + 1) }) {
        R::Ok(v) => v + x,
        R::Err(e) => e,
    }
}
