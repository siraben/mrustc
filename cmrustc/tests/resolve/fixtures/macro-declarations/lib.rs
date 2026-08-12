#[rustc_builtin_macro]
#[macro_export]
macro_rules! include {
    ($file:expr) => {};
}

mod nested {
    #[doc = "opaque"]
    pub macro opaque($token:tt) {
        $token
    }

    #[cfg(windows)]
    macro_rules! hidden {
        () => {};
    }
}
