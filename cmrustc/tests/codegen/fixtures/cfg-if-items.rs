#![feature(no_core)]
#![no_core]
#![no_main]

// libc's crate-root shape: a `cfg_if!` cascade whose bodies are items
// (`mod`, `pub use`, a `prelude!()` invocation) — the `$it:item` fragments
// must span whole items, and generated `use` items are imports.
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
    (
        if #[cfg($($i_met:meta),*)] { $($i_it:item)* }
        $(
            else if #[cfg($($e_met:meta),*)] { $($e_it:item)* }
        )*
    ) => {
        cfg_if! {
            @__items
            () ;
            ( ($($i_met),*) ($($i_it)*) ),
            $( ( ($($e_met),*) ($($e_it)*) ), )*
            ( () () ),
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

macro_rules! prelude {
    () => {
        mod prelude {
            pub(crate) const PRELUDE: u32 = 7;
        }
    };
}

cfg_if! {
    if #[cfg(feature = "nope")] {
        fn never() -> u32 { 0 }
    }
}

cfg_if! {
    if #[cfg(windows)] {
        mod windows { pub const BASE: u32 = 1; }
        pub use crate::windows::*;
        prelude!();
    } else if #[cfg(target_os = "linux")] {
        mod primitives { pub const BASE: u32 = 100; }
        pub use crate::primitives::*;
        prelude!();
    } else {
        mod other { pub const BASE: u32 = 5; }
        pub use crate::other::*;
        prelude!();
    }
}

#[no_mangle]
pub extern "C" fn cfg_if_items(x: u32) -> u32 {
    x + BASE + prelude::PRELUDE
}
