#[cfg(unix)]
#[allow(dead_code)]
pub struct Included;

#[cfg(windows)]
pub struct Hidden;

pub fn included_function() {}
