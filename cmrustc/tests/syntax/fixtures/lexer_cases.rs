//! Inner documentation is trivia, but must retain its style.

/* outer /* nested */ still outer */
pub async fn lex<'input>(value: &'input str) -> (u32, &'static str) {
    let r#type = 0xff_u32;
    let decimal = 12_345usize;
    let float = 1.25e-2f64;
    let chars = ('x', '\n', b'z');
    let strings = (
        "cooked\\nstring",
        b"bytes",
        c"C string",
        r#"raw " string"#,
        br##"raw byte # string"##,
        cr###"raw C ## string"###,
    );
    if value != "" && r#type >= 1 {
        (decimal as u32, 'label: loop { break 'label "done"; })
    } else {
        (0, "")
    }
}
