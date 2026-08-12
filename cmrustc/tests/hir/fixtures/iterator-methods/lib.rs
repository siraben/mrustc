pub trait Sized {}

// The attributes and declaration headers below are copied from Rust 1.90
// core's Iterator trait. Default bodies are deliberately minimized to a
// diverging expression: this fixture tests declaration admission, while
// retaining the upstream required-versus-default body distinction.
#[stable(feature = "rust1", since = "1.0.0")]
#[rustc_on_unimplemented(
    on(
        Self = "core::ops::range::RangeTo<Idx>",
        note = "you might have meant to use a bounded `Range`"
    ),
    on(
        Self = "core::ops::range::RangeToInclusive<Idx>",
        note = "you might have meant to use a bounded `RangeInclusive`"
    ),
    label = "`{Self}` is not an iterator",
    message = "`{Self}` is not an iterator"
)]
#[doc(notable_trait)]
#[lang = "iterator"]
#[rustc_diagnostic_item = "Iterator"]
#[must_use = "iterators are lazy and do nothing unless consumed"]
pub trait Iterator {
    #[rustc_diagnostic_item = "IteratorItem"]
    #[stable(feature = "rust1", since = "1.0.0")]
    type Item;

    #[lang = "next"]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn next(&mut self) -> Option<Self::Item>;

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn size_hint(&self) -> (usize, Option<usize>) {
        loop {}
    }

    #[inline]
    #[unstable(feature = "iter_advance_by", reason = "recently added", issue = "77404")]
    fn advance_by(&mut self, n: usize) -> Result<(), NonZero<usize>> {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn nth(&mut self, n: usize) -> Option<Self::Item> {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn count(self) -> usize
    where
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn last(self) -> Option<Self::Item>
    where
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iterator_step_by", since = "1.28.0")]
    fn step_by(self, step: usize) -> StepBy<Self>
    where
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    #[rustc_diagnostic_item = "enumerate_method"]
    fn enumerate(self) -> Enumerate<Self>
    where
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn peekable(self) -> Peekable<Self>
    where
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn skip(self, n: usize) -> Skip<Self>
    where
        Self: Sized,
    {
        loop {}
    }

    #[doc(alias = "limit")]
    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn take(self, n: usize) -> Take<Self>
    where
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn fuse(self) -> Fuse<Self>
    where
        Self: Sized,
    {
        loop {}
    }

    #[stable(feature = "rust1", since = "1.0.0")]
    fn by_ref(&mut self) -> &mut Self
    where
        Self: Sized,
    {
        self
    }
}

pub enum Option<T> {
    None,
    Some(T),
}

pub enum Result<T, E> {
    Ok(T),
    Err(E),
}

pub struct NonZero<T>(T);
pub struct StepBy<I>(I);
pub struct Enumerate<I>(I);
pub struct Peekable<I>(I);
pub struct Skip<I>(I);
pub struct Take<I>(I);
pub struct Fuse<I>(I);
