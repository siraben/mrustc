enum Inner<'a> {
    Bytes(&'a [u8]),
    Scalar(u32),
}

enum Outer<'a> {
    Empty,
    Value(Inner<'a>),
}

#[no_mangle]
pub extern "C" fn nested_variant_pattern_probe(which: u32) -> u32 {
    let bytes = [7u8];
    let value = if which == 0 {
        Outer::Value(Inner::Bytes(&bytes))
    } else {
        Outer::Value(Inner::Scalar(38))
    };
    match value {
        Outer::Value(Inner::Bytes([byte])) => *byte as u32,
        Outer::Value(Inner::Scalar(number)) => number,
        _ => 0,
    }
}
