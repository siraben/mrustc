#![feature(no_core)]
#![no_core]
#![no_main]

// std's BufWriter::flush_buf: a body-local struct with body-local impl
// blocks (methods and Drop), used through its methods.
#[lang = "sized"]
trait Sized {}
#[lang = "drop"]
trait Drop {
    fn drop(&mut self);
}

static mut DROPPED: u32 = 0;

#[no_mangle]
pub extern "C" fn local_impl(x: u32) -> u32 {
    struct Guard<'a> {
        slot: &'a mut u32,
        written: u32,
    }

    impl<'a> Guard<'a> {
        fn new(slot: &'a mut u32) -> Self {
            Self { slot, written: 0 }
        }
        fn remaining(&self) -> u32 {
            *self.slot - self.written
        }
        fn consume(&mut self, amt: u32) {
            self.written = self.written + amt;
        }
        fn done(&self) -> bool {
            self.written >= *self.slot
        }
    }

    impl Drop for Guard<'_> {
        fn drop(&mut self) {
            unsafe { DROPPED = DROPPED + 1; }
        }
    }

    let mut total = x;
    let mut guard = Guard::new(&mut total);
    let mut steps = 0;
    while !guard.done() {
        let r = guard.remaining();
        guard.consume(if r > 2 { 2 } else { r });
        steps = steps + 1;
    }
    steps * 10 + guard.written
}
