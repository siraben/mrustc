#![feature(no_core)]
#![no_core]
#![no_main]

// std's print path: `self.lock().write_fmt(args)` reaches Write's
// provided `write_fmt` for StdoutLock, which calls a generic helper,
// which calls the provided `write_all`, which calls the required
// `write` -- every provided-method instance must carry its Self.
trait Write {
    fn write(&mut self, x: u32) -> u32;
    fn write_all(&mut self, x: u32) -> u32 { self.write(x) + 1 }
    fn write_fmt(&mut self, x: u32) -> u32 { default_write_fmt(self, x) }
}

fn default_write_fmt<T: Write + ?Sized>(this: &mut T, x: u32) -> u32 {
    let mut a = Adapter { inner: this };
    a.write_str(x)
}

struct Adapter<'a, T: ?Sized> {
    inner: &'a mut T,
}

trait FmtWrite {
    fn write_str(&mut self, x: u32) -> u32;
}

impl<T: Write + ?Sized> FmtWrite for Adapter<'_, T> {
    fn write_str(&mut self, x: u32) -> u32 { self.inner.write_all(x) }
}

struct Lock {
    n: u32,
}

impl Write for Lock {
    fn write(&mut self, x: u32) -> u32 { self.n = self.n + x; self.n }
}

struct Out {
    base: u32,
}

impl Out {
    fn lock(&self) -> Lock { Lock { n: self.base } }
}

impl Write for &Out {
    fn write(&mut self, x: u32) -> u32 { self.lock().write(x) }
    fn write_fmt(&mut self, x: u32) -> u32 { self.lock().write_fmt(x) }
}

fn print_to<T: Write>(x: u32, global: fn() -> T) -> u32 {
    global().write_fmt(x)
}

static OUT: Out = Out { base: 100 };

fn out() -> &'static Out { &OUT }

#[no_mangle]
pub extern "C" fn trait_default_self(x: u32) -> u32 {
    print_to(x, out)
}
