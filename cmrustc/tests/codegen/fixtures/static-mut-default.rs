#![feature(no_core)]
#![no_core]
#![no_main]

// std's `static mut THREAD_INFO: BTreeMap<..> = BTreeMap::new();`: a
// mutable static is a cached slot written through its address. (std's
// `Report<E = Box<dyn Error>>` default with an elided object lifetime is
// exercised by the std chain; a local trait in a default is a separate,
// older ordering gap.)
trait Error {
    fn code(&self) -> u32;
}

struct Report<E = Plain> {
    error: E,
}

struct Plain(u32);

impl Error for Plain {
    fn code(&self) -> u32 { self.0 }
}

static mut COUNTER: u32 = 5;

fn bump(by: u32) -> u32 {
    unsafe {
        COUNTER = COUNTER + by;
        COUNTER
    }
}

#[no_mangle]
pub extern "C" fn static_mut_default(x: u32) -> u32 {
    let r: Report<Plain> = Report { error: Plain(x) };
    bump(1);
    bump(2) + r.error.code()
}
