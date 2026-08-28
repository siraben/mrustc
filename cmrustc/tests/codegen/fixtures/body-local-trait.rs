#![feature(no_core)]
#![no_core]
#![no_main]

// core's Write::write_fmt declares a trait and an impl for `&mut W` inside
// its own body and dispatches through them.
trait Write {
    fn write_val(&mut self, value: u32) -> u32;

    fn write_all(&mut self, value: u32) -> u32 {
        trait SpecWrite {
            fn spec_write(self, value: u32) -> u32;
        }

        impl<W: Write + ?Sized> SpecWrite for &mut W {
            fn spec_write(self, value: u32) -> u32 {
                self.write_val(value + 1)
            }
        }

        self.spec_write(value)
    }
}

struct Acc {
    total: u32,
}

impl Write for Acc {
    fn write_val(&mut self, value: u32) -> u32 {
        self.total = self.total + value;
        self.total
    }
}

#[no_mangle]
pub extern "C" fn body_local_trait(start: u32, value: u32) -> u32 {
    let mut acc = Acc { total: start };
    acc.write_all(value)
}
