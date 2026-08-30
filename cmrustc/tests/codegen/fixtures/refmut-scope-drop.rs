#![feature(no_core)]
#![no_core]
#![no_main]

#[lang = "sized"]
trait Sized {}

#[lang = "drop"]
trait Drop {
    fn drop(&mut self);
}

mod cell {
    static mut BORROWS: u32 = 0;
    static mut DROPS: u32 = 0;

    struct BorrowRefMut {
        token: u32,
    }

    impl crate::Drop for BorrowRefMut {
        fn drop(&mut self) {
            unsafe {
                BORROWS = BORROWS - 1;
                DROPS = DROPS + 1;
            }
        }
    }

    // Like core::cell::RefMut, the guard itself has no Drop impl: cleanup is
    // carried by its BorrowRefMut field.
    struct RefMut {
        value: u32,
        borrow: BorrowRefMut,
    }

    pub fn run() -> u32 {
        unsafe { BORROWS = BORROWS + 1; }
        {
            let guard = RefMut {
                value: 9,
                borrow: BorrowRefMut { token: 1 },
            };
            let _value = guard.value;
        }
        unsafe { BORROWS + DROPS * 10 }
    }
}

#[no_mangle]
pub extern "C" fn refmut_scope_drop_probe() -> u32 {
    cell::run()
}
