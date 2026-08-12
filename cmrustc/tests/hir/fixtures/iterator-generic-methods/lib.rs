// This fixture isolates Iterator declaration admission. The Iterator trait
// attributes, method attributes, and method headers are copied verbatim from
// rust-lang/rust 1.90.0, library/core/src/iter/traits/iterator.rs. Upstream
// default bodies are replaced only by `loop {}`;
// required declarations remain bodyless.
//
// The nine deliberately excluded methods exercise const generics (next_chunk,
// map_windows, array_chunks), lifetime generics (partition_in_place, copied,
// cloned), or nested associated bounds / impl Trait (try_collect, try_reduce,
// try_find).
//
// The support declarations below are intentionally skeletal: they preserve
// only the names, generic arities, associated types, and supertrait relations
// referenced by those headers. They do not model library behavior.

pub trait Sized {}
pub trait Clone {}
pub trait Default {}
pub trait PartialEq<Rhs = Self> {}
pub trait PartialOrd<Rhs = Self>: PartialEq<Rhs> {}
pub trait Ord: PartialOrd<Self> {}
pub trait FromIterator<A> {}
pub trait Extend<A> {}
pub trait Sum<A> {}
pub trait Product<A> {}
pub trait IntoIterator {
    type Item;
    type IntoIter;
}
pub trait FnOnce<Args> {
    type Output;
}
pub trait FnMut<Args>: FnOnce<Args> {}
pub trait Try {
    type Output;
}
pub trait TrustedRandomAccessNoCoerce {}

pub enum Option<T> {
    None,
    Some(T),
}
pub enum Result<T, E> {
    Ok(T),
    Err(E),
}
pub enum Ordering {
    Less,
    Equal,
    Greater,
}
pub struct NonZero<T>(T);

pub struct StepBy<I>(I);
pub struct Chain<I, J>(I, J);
pub struct Zip<I, J>(I, J);
pub struct Intersperse<I>(I);
pub struct IntersperseWith<I, G>(I, G);
pub struct Map<I, F>(I, F);
pub struct Filter<I, P>(I, P);
pub struct FilterMap<I, F>(I, F);
pub struct Enumerate<I>(I);
pub struct Peekable<I>(I);
pub struct SkipWhile<I, P>(I, P);
pub struct TakeWhile<I, P>(I, P);
pub struct MapWhile<I, P>(I, P);
pub struct Skip<I>(I);
pub struct Take<I>(I);
pub struct Scan<I, St, F>(I, St, F);
pub struct FlatMap<I, U, F>(I, U, F);
pub struct Flatten<I>(I);
pub struct Fuse<I>(I);
pub struct Inspect<I, F>(I, F);
pub struct Rev<I>(I);
pub struct Cycle<I>(I);

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
    #[stable(feature = "iterator_step_by", since = "1.28.0")]
    fn step_by(self, step: usize) -> StepBy<Self>
    where
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn chain<U>(self, other: U) -> Chain<Self, U::IntoIter>
    where
        Self: Sized,
        U: IntoIterator<Item = Self::Item>,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn zip<U>(self, other: U) -> Zip<Self, U::IntoIter>
    where
        Self: Sized,
        U: IntoIterator,
    {
        loop {}
    }

    #[inline]
    #[unstable(feature = "iter_intersperse", reason = "recently added", issue = "79524")]
    fn intersperse(self, separator: Self::Item) -> Intersperse<Self>
    where
        Self: Sized,
        Self::Item: Clone,
    {
        loop {}
    }

    #[inline]
    #[unstable(feature = "iter_intersperse", reason = "recently added", issue = "79524")]
    fn intersperse_with<G>(self, separator: G) -> IntersperseWith<Self, G>
    where
        Self: Sized,
        G: FnMut() -> Self::Item,
    {
        loop {}
    }

    #[rustc_diagnostic_item = "IteratorMap"]
    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn map<B, F>(self, f: F) -> Map<Self, F>
    where
        Self: Sized,
        F: FnMut(Self::Item) -> B,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iterator_for_each", since = "1.21.0")]
    fn for_each<F>(self, f: F)
    where
        Self: Sized,
        F: FnMut(Self::Item),
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    #[rustc_diagnostic_item = "iter_filter"]
    fn filter<P>(self, predicate: P) -> Filter<Self, P>
    where
        Self: Sized,
        P: FnMut(&Self::Item) -> bool,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn filter_map<B, F>(self, f: F) -> FilterMap<Self, F>
    where
        Self: Sized,
        F: FnMut(Self::Item) -> Option<B>,
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
    #[doc(alias = "drop_while")]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn skip_while<P>(self, predicate: P) -> SkipWhile<Self, P>
    where
        Self: Sized,
        P: FnMut(&Self::Item) -> bool,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn take_while<P>(self, predicate: P) -> TakeWhile<Self, P>
    where
        Self: Sized,
        P: FnMut(&Self::Item) -> bool,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iter_map_while", since = "1.57.0")]
    fn map_while<B, P>(self, predicate: P) -> MapWhile<Self, P>
    where
        Self: Sized,
        P: FnMut(Self::Item) -> Option<B>,
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
    fn scan<St, B, F>(self, initial_state: St, f: F) -> Scan<Self, St, F>
    where
        Self: Sized,
        F: FnMut(&mut St, Self::Item) -> Option<B>,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn flat_map<U, F>(self, f: F) -> FlatMap<Self, U, F>
    where
        Self: Sized,
        U: IntoIterator,
        F: FnMut(Self::Item) -> U,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iterator_flatten", since = "1.29.0")]
    fn flatten(self) -> Flatten<Self>
    where
        Self: Sized,
        Self::Item: IntoIterator,
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

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn inspect<F>(self, f: F) -> Inspect<Self, F>
    where
        Self: Sized,
        F: FnMut(&Self::Item),
    {
        loop {}
    }

    #[stable(feature = "rust1", since = "1.0.0")]
    fn by_ref(&mut self) -> &mut Self
    where
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    #[must_use = "if you really need to exhaust the iterator, consider `.for_each(drop)` instead"]
    #[rustc_diagnostic_item = "iterator_collect_fn"]
    fn collect<B: FromIterator<Self::Item>>(self) -> B
    where
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[unstable(feature = "iter_collect_into", reason = "new API", issue = "94780")]
    fn collect_into<E: Extend<Self::Item>>(self, collection: &mut E) -> &mut E
    where
        Self: Sized,
    {
        loop {}
    }

    #[stable(feature = "rust1", since = "1.0.0")]
    fn partition<B, F>(self, f: F) -> (B, B)
    where
        Self: Sized,
        B: Default + Extend<Self::Item>,
        F: FnMut(&Self::Item) -> bool,
    {
        loop {}
    }

    #[unstable(feature = "iter_is_partitioned", reason = "new API", issue = "62544")]
    fn is_partitioned<P>(mut self, mut predicate: P) -> bool
    where
        Self: Sized,
        P: FnMut(Self::Item) -> bool,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iterator_try_fold", since = "1.27.0")]
    fn try_fold<B, F, R>(&mut self, init: B, mut f: F) -> R
    where
        Self: Sized,
        F: FnMut(B, Self::Item) -> R,
        R: Try<Output = B>,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iterator_try_fold", since = "1.27.0")]
    fn try_for_each<F, R>(&mut self, f: F) -> R
    where
        Self: Sized,
        F: FnMut(Self::Item) -> R,
        R: Try<Output = ()>,
    {
        loop {}
    }

    #[doc(alias = "inject", alias = "foldl")]
    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn fold<B, F>(mut self, init: B, mut f: F) -> B
    where
        Self: Sized,
        F: FnMut(B, Self::Item) -> B,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iterator_fold_self", since = "1.51.0")]
    fn reduce<F>(mut self, f: F) -> Option<Self::Item>
    where
        Self: Sized,
        F: FnMut(Self::Item, Self::Item) -> Self::Item,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn all<F>(&mut self, f: F) -> bool
    where
        Self: Sized,
        F: FnMut(Self::Item) -> bool,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn any<F>(&mut self, f: F) -> bool
    where
        Self: Sized,
        F: FnMut(Self::Item) -> bool,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn find<P>(&mut self, predicate: P) -> Option<Self::Item>
    where
        Self: Sized,
        P: FnMut(&Self::Item) -> bool,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iterator_find_map", since = "1.30.0")]
    fn find_map<B, F>(&mut self, f: F) -> Option<B>
    where
        Self: Sized,
        F: FnMut(Self::Item) -> Option<B>,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn position<P>(&mut self, predicate: P) -> Option<usize>
    where
        Self: Sized,
        P: FnMut(Self::Item) -> bool,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn rposition<P>(&mut self, predicate: P) -> Option<usize>
    where
        P: FnMut(Self::Item) -> bool,
        Self: Sized + ExactSizeIterator + DoubleEndedIterator,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn max(self) -> Option<Self::Item>
    where
        Self: Sized,
        Self::Item: Ord,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn min(self) -> Option<Self::Item>
    where
        Self: Sized,
        Self::Item: Ord,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iter_cmp_by_key", since = "1.6.0")]
    fn max_by_key<B: Ord, F>(self, f: F) -> Option<Self::Item>
    where
        Self: Sized,
        F: FnMut(&Self::Item) -> B,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iter_max_by", since = "1.15.0")]
    fn max_by<F>(self, compare: F) -> Option<Self::Item>
    where
        Self: Sized,
        F: FnMut(&Self::Item, &Self::Item) -> Ordering,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iter_cmp_by_key", since = "1.6.0")]
    fn min_by_key<B: Ord, F>(self, f: F) -> Option<Self::Item>
    where
        Self: Sized,
        F: FnMut(&Self::Item) -> B,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "iter_min_by", since = "1.15.0")]
    fn min_by<F>(self, compare: F) -> Option<Self::Item>
    where
        Self: Sized,
        F: FnMut(&Self::Item, &Self::Item) -> Ordering,
    {
        loop {}
    }

    #[inline]
    #[doc(alias = "reverse")]
    #[stable(feature = "rust1", since = "1.0.0")]
    fn rev(self) -> Rev<Self>
    where
        Self: Sized + DoubleEndedIterator,
    {
        loop {}
    }

    #[stable(feature = "rust1", since = "1.0.0")]
    fn unzip<A, B, FromA, FromB>(self) -> (FromA, FromB)
    where
        FromA: Default + Extend<A>,
        FromB: Default + Extend<B>,
        Self: Sized + Iterator<Item = (A, B)>,
    {
        loop {}
    }

    #[stable(feature = "rust1", since = "1.0.0")]
    #[inline]
    fn cycle(self) -> Cycle<Self>
    where
        Self: Sized + Clone,
    {
        loop {}
    }

    #[stable(feature = "iter_arith", since = "1.11.0")]
    fn sum<S>(self) -> S
    where
        Self: Sized,
        S: Sum<Self::Item>,
    {
        loop {}
    }

    #[stable(feature = "iter_arith", since = "1.11.0")]
    fn product<P>(self) -> P
    where
        Self: Sized,
        P: Product<Self::Item>,
    {
        loop {}
    }

    #[stable(feature = "iter_order", since = "1.5.0")]
    fn cmp<I>(self, other: I) -> Ordering
    where
        I: IntoIterator<Item = Self::Item>,
        Self::Item: Ord,
        Self: Sized,
    {
        loop {}
    }

    #[unstable(feature = "iter_order_by", issue = "64295")]
    fn cmp_by<I, F>(self, other: I, cmp: F) -> Ordering
    where
        Self: Sized,
        I: IntoIterator,
        F: FnMut(Self::Item, I::Item) -> Ordering,
    {
        loop {}
    }

    #[stable(feature = "iter_order", since = "1.5.0")]
    fn partial_cmp<I>(self, other: I) -> Option<Ordering>
    where
        I: IntoIterator,
        Self::Item: PartialOrd<I::Item>,
        Self: Sized,
    {
        loop {}
    }

    #[unstable(feature = "iter_order_by", issue = "64295")]
    fn partial_cmp_by<I, F>(self, other: I, partial_cmp: F) -> Option<Ordering>
    where
        Self: Sized,
        I: IntoIterator,
        F: FnMut(Self::Item, I::Item) -> Option<Ordering>,
    {
        loop {}
    }

    #[stable(feature = "iter_order", since = "1.5.0")]
    fn eq<I>(self, other: I) -> bool
    where
        I: IntoIterator,
        Self::Item: PartialEq<I::Item>,
        Self: Sized,
    {
        loop {}
    }

    #[unstable(feature = "iter_order_by", issue = "64295")]
    fn eq_by<I, F>(self, other: I, eq: F) -> bool
    where
        Self: Sized,
        I: IntoIterator,
        F: FnMut(Self::Item, I::Item) -> bool,
    {
        loop {}
    }

    #[stable(feature = "iter_order", since = "1.5.0")]
    fn ne<I>(self, other: I) -> bool
    where
        I: IntoIterator,
        Self::Item: PartialEq<I::Item>,
        Self: Sized,
    {
        loop {}
    }

    #[stable(feature = "iter_order", since = "1.5.0")]
    fn lt<I>(self, other: I) -> bool
    where
        I: IntoIterator,
        Self::Item: PartialOrd<I::Item>,
        Self: Sized,
    {
        loop {}
    }

    #[stable(feature = "iter_order", since = "1.5.0")]
    fn le<I>(self, other: I) -> bool
    where
        I: IntoIterator,
        Self::Item: PartialOrd<I::Item>,
        Self: Sized,
    {
        loop {}
    }

    #[stable(feature = "iter_order", since = "1.5.0")]
    fn gt<I>(self, other: I) -> bool
    where
        I: IntoIterator,
        Self::Item: PartialOrd<I::Item>,
        Self: Sized,
    {
        loop {}
    }

    #[stable(feature = "iter_order", since = "1.5.0")]
    fn ge<I>(self, other: I) -> bool
    where
        I: IntoIterator,
        Self::Item: PartialOrd<I::Item>,
        Self: Sized,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "is_sorted", since = "1.82.0")]
    fn is_sorted(self) -> bool
    where
        Self: Sized,
        Self::Item: PartialOrd,
    {
        loop {}
    }

    #[stable(feature = "is_sorted", since = "1.82.0")]
    fn is_sorted_by<F>(mut self, compare: F) -> bool
    where
        Self: Sized,
        F: FnMut(&Self::Item, &Self::Item) -> bool,
    {
        loop {}
    }

    #[inline]
    #[stable(feature = "is_sorted", since = "1.82.0")]
    fn is_sorted_by_key<F, K>(self, f: F) -> bool
    where
        Self: Sized,
        F: FnMut(Self::Item) -> K,
        K: PartialOrd,
    {
        loop {}
    }

    #[inline]
    #[doc(hidden)]
    #[unstable(feature = "trusted_random_access", issue = "none")]
    unsafe fn __iterator_get_unchecked(&mut self, _idx: usize) -> Self::Item
    where
        Self: TrustedRandomAccessNoCoerce,
    {
        loop {}
    }

}

pub trait DoubleEndedIterator: Iterator {}
pub trait ExactSizeIterator: Iterator {}
