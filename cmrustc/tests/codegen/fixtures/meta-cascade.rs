#![feature(no_core)]
#![no_core]
#![no_main]

// std's sys/random/mod.rs shape: cfg-if's `@__items` recursion carries a
// growing `( $( $no:meta , )* )` list before a failing tail, so `meta`
// fragments must have one deterministic extent (searching every extent
// explores 2^n partitions at 20 branches).
macro_rules! cfg_if {
    // match if/else chains with a final `else`
    (
        $(
            if #[cfg( $i_meta:meta )] { $( $i_tokens:tt )* }
        ) else+
        else { $( $e_tokens:tt )* }
    ) => {
        cfg_if! {
            @__items () ;
            $(
                (( $i_meta ) ( $( $i_tokens )* )) ,
            )+
            (() ( $( $e_tokens )* )) ,
        }
    };

    // match if/else chains lacking a final `else`
    (
        if #[cfg( $i_meta:meta )] { $( $i_tokens:tt )* }
        $(
            else if #[cfg( $e_meta:meta )] { $( $e_tokens:tt )* }
        )*
    ) => {
        cfg_if! {
            @__items () ;
            (( $i_meta ) ( $( $i_tokens )* )) ,
            $(
                (( $e_meta ) ( $( $e_tokens )* )) ,
            )*
        }
    };

    // Internal and recursive macro to emit all the items
    //
    // Collects all the previous cfgs in a list at the beginning, so they can be
    // negated. After the semicolon are all the remaining items.
    (@__items ( $( $_:meta , )* ) ; ) => {};
    (
        @__items ( $( $no:meta , )* ) ;
        (( $( $yes:meta )? ) ( $( $tokens:tt )* )) ,
        $( $rest:tt , )*
    ) => {
        // Emit all items within one block, applying an appropriate #[cfg]. The
        // #[cfg] will require all `$yes` matchers specified and must also negate
        // all previous matchers.
        #[cfg(all(
            $( $yes , )?
            not(any( $( $no ),* ))
        ))]
        cfg_if! { @__identity $( $tokens )* }

        // Recurse to emit all other items in `$rest`, and when we do so add all
        // our `$yes` matchers to the list of `$no` matchers as future emissions
        // will have to negate everything we just matched as well.
        cfg_if! {
            @__items ( $( $no , )* $( $yes , )? ) ;
            $( $rest , )*
        }
    };

    // Internal macro to make __apply work out right for different match types,
    // because of how macros match/expand stuff.
    (@__identity $( $tokens:tt )* ) => {
        $( $tokens )*
    };
}

cfg_if! {
    if #[cfg(target_os = "os0")] {
        fn base() -> u32 { 0 }
    } else if #[cfg(target_os = "os1")] {
        fn base() -> u32 { 1 }
    } else if #[cfg(target_os = "os2")] {
        fn base() -> u32 { 2 }
    } else if #[cfg(target_os = "os3")] {
        fn base() -> u32 { 3 }
    } else if #[cfg(target_os = "os4")] {
        fn base() -> u32 { 4 }
    } else if #[cfg(target_os = "os5")] {
        fn base() -> u32 { 5 }
    } else if #[cfg(target_os = "os6")] {
        fn base() -> u32 { 6 }
    } else if #[cfg(target_os = "os7")] {
        fn base() -> u32 { 7 }
    } else if #[cfg(target_os = "os8")] {
        fn base() -> u32 { 8 }
    } else if #[cfg(target_os = "os9")] {
        fn base() -> u32 { 9 }
    } else if #[cfg(target_os = "os10")] {
        fn base() -> u32 { 10 }
    } else if #[cfg(any(target_os = "linux", target_os = "android"))] {
        fn base() -> u32 { 11 }
    } else if #[cfg(target_os = "os12")] {
        fn base() -> u32 { 12 }
    } else if #[cfg(target_os = "os13")] {
        fn base() -> u32 { 13 }
    } else if #[cfg(target_os = "os14")] {
        fn base() -> u32 { 14 }
    } else if #[cfg(target_os = "os15")] {
        fn base() -> u32 { 15 }
    } else if #[cfg(target_os = "os16")] {
        fn base() -> u32 { 16 }
    } else if #[cfg(target_os = "os17")] {
        fn base() -> u32 { 17 }
    } else if #[cfg(target_os = "os18")] {
        fn base() -> u32 { 18 }
    } else if #[cfg(target_os = "os19")] {
        fn base() -> u32 { 19 }
    }
}

#[no_mangle]
pub extern "C" fn meta_cascade(x: u32) -> u32 {
    x + base()
}
