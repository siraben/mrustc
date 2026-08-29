#![feature(no_core)]
#![feature(decl_macro)]
#![no_core]
#![no_main]

// std's thread_local plumbing: `thread_local!` (#[macro_export], seen through
// `#[macro_use] mod thread`) expands to `$crate::thread::local_impl::
// thread_local_inner!`, a *generated qualified* path through a glob re-export
// of a `pub use native::{thread_local_inner}` inside a cfg_if!-generated
// unit, reaching a `pub macro` (decl macro 2.0); `local_pointer` is a
// `pub(crate) macro` re-exported the same way and `use`-imported. The
// second thread_local! sits in a cfg_if! arm (a generated unit).

macro_rules! cfg_if {
    ($(
        if #[cfg($($meta:meta),*)] { $($it:item)* }
    ) else * else {
        $($it2:item)*
    }) => {
        cfg_if! {
            @__items
            () ;
            $( ( ($($meta),*) ($($it)*) ), )*
            ( () ($($it2)*) ),
        }
    };
    (@__items ($($not:meta,)*) ; ) => {};
    (@__items ($($not:meta,)*) ; ( ($($m:meta),*) ($($it:item)*) ),
     $($rest:tt)*) => {
        cfg_if! { @__apply cfg(all($($m,)* not(any($($not),*)))), $($it)* }
        cfg_if! { @__items ($($not,)* $($m,)*) ; $($rest)* }
    };
    (@__apply $m:meta, $($it:item)*) => {
        $(#[$m] $it)*
    };
}

#[macro_use]
pub mod thread {
    #[macro_export]
    macro_rules! thread_local {
        ($vis:vis static $name:ident: $t:ty = $init:expr) => {
            $crate::thread::local_impl::thread_local_inner!($vis $name, $t, $init);
        };
    }
    pub mod local_impl {
        pub use crate::sys::thread_local::*;
    }
}

mod sys {
    pub mod thread_local {
        cfg_if! {
            if #[cfg(target_os = "linux")] {
                mod native {
                    pub macro thread_local_inner {
                        ($vis:vis $name:ident, $t:ty, $init:expr) => {
                            $vis const $name: $t = $init + 100;
                        }
                    }
                    pub(crate) macro local_pointer {
                        ($name:ident, $t:ty) => {
                            pub struct $name { pub v: $t }
                        }
                    }
                }
                pub use native::{thread_local_inner};
                pub(crate) use native::{local_pointer};
            } else {
                mod other {
                    pub macro thread_local_inner {
                        ($vis:vis $name:ident, $t:ty, $init:expr) => {
                            $vis const $name: $t = $init + 200;
                        }
                    }
                    pub(crate) macro local_pointer {
                        ($name:ident, $t:ty) => {
                            pub struct $name { pub v: $t }
                        }
                    }
                }
                pub use other::{thread_local_inner};
                pub(crate) use other::{local_pointer};
            }
        }
    }
}

mod user {
    use crate::sys::thread_local::local_pointer;
    local_pointer! { Key, u32 }
    thread_local! { pub static COUNT: u32 = 7 }
    cfg_if! {
        if #[cfg(target_os = "linux")] {
            thread_local! { pub static COUNT2: u32 = 1000 }
        } else {
            thread_local! { pub static COUNT2: u32 = 2000 }
        }
    }
    pub fn read() -> u32 { let k = Key { v: 3 }; COUNT + k.v + COUNT2 }
}

#[no_mangle]
pub extern "C" fn thread_local_plumb() -> u32 {
    user::read()
}
