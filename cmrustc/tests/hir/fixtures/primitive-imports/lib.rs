pub mod primitive {
    pub use bool;
    pub use u8;
}

use primitive::u8 as Byte;

pub struct UsesPrimitives {
    pub qualified: primitive::bool,
    pub alias: Byte,
    pub absolute: crate::primitive::u8,
}
