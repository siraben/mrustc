unsafe auto trait Send {}

impl<T> !Send for *const T {}
