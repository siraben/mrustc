pub trait Gate<T: ?Sized> {}

pub use Gate as GateReexport;

pub fn needs<X: Gate<u8>>() {}
