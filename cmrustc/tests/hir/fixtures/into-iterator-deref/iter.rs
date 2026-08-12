pub trait Iterator {
    type Item;
}

#[rustc_diagnostic_item = "IntoIterator"]
#[rustc_on_unimplemented(
    on(
        Self = "core::ops::range::RangeTo<Idx>",
        label = "if you meant to iterate until a value, add a starting value",
        note = "`..end` is a `RangeTo`, which cannot be iterated on; you might have meant to have a \
              bounded `Range`: `0..end`"
    ),
    on(
        Self = "core::ops::range::RangeToInclusive<Idx>",
        label = "if you meant to iterate until a value (including it), add a starting value",
        note = "`..=end` is a `RangeToInclusive`, which cannot be iterated on; you might have meant \
              to have a bounded `RangeInclusive`: `0..=end`"
    ),
    on(
        Self = "[]",
        label = "`{Self}` is not an iterator; try calling `.into_iter()` or `.iter()`"
    ),
    on(Self = "&[]", label = "`{Self}` is not an iterator; try calling `.iter()`"),
    on(
        Self = "alloc::vec::Vec<T, A>",
        label = "`{Self}` is not an iterator; try calling `.into_iter()` or `.iter()`"
    ),
    on(Self = "&str", label = "`{Self}` is not an iterator; try calling `.chars()` or `.bytes()`"),
    on(
        Self = "alloc::string::String",
        label = "`{Self}` is not an iterator; try calling `.chars()` or `.bytes()`"
    ),
    on(
        Self = "{integral}",
        note = "if you want to iterate between `start` until a value `end`, use the exclusive range \
              syntax `start..end` or the inclusive range syntax `start..=end`"
    ),
    on(
        Self = "{float}",
        note = "if you want to iterate between `start` until a value `end`, use the exclusive range \
              syntax `start..end` or the inclusive range syntax `start..=end`"
    ),
    label = "`{Self}` is not an iterator",
    message = "`{Self}` is not an iterator"
)]
#[rustc_skip_during_method_dispatch(array, boxed_slice)]
#[stable(feature = "rust1", since = "1.0.0")]
pub trait IntoIterator {
    #[stable(feature = "rust1", since = "1.0.0")]
    type Item;

    #[stable(feature = "rust1", since = "1.0.0")]
    type IntoIter: Iterator<Item = Self::Item>;

    #[lang = "into_iter"]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn into_iter(self) -> Self::IntoIter;
}
