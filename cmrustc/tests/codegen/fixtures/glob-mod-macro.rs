#![feature(no_core)]
#![feature(decl_macro)]
#![no_core]
#![no_main]

// std's `use crate::sys::weak::dlsym;` where `sys` reaches `weak` through
// `pub use pal::unix::*`: a later path segment may name a glob-imported
// module, not only a child module.
mod sys {
    pub mod pal {
        pub mod unix {
            pub mod weak {
                pub(crate) macro dlsym {
                    (fn $name:ident() -> $ret:ty;) => {
                        fn $name() -> $ret { 9 }
                    }
                }
            }
        }
    }
    pub use self::pal::unix::*;
}

fn go() -> u32 {
    let v = {
        use crate::sys::weak::dlsym;
        dlsym!(fn nine() -> u32;);
        nine()
    };
    v + 2
}

#[no_mangle]
pub extern "C" fn glob_mod_macro() -> u32 {
    go()
}
