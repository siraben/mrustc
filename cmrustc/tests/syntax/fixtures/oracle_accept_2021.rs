#![allow(dead_code)]

/* outer /* nested */ still outer */
pub async fn accepted<'input>(value: &'input str) {
    let r#type = 0xff_u32;
    let _number = 12_345usize;
    let _float = 1.25e-2f64;
    let _chars = ('x', '\n', b'z');
    let _strings = (
        "cooked\\nstring",
        b"bytes",
        c"C string",
        r#"raw " string"#,
        br##"raw byte # string"##,
    );
    'label: loop {
        if value != "" && r#type >= 1 {
            break 'label;
        }
        break;
    }
}
