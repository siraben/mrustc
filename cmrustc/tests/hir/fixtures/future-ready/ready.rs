use crate::future::Future;
use crate::pin::Pin;
use crate::task::{Context, Poll};

pub struct Ready<T>(T);

#[stable(feature = "future_readiness_fns", since = "1.48.0")]
impl<T> Future for Ready<T> {
    type Output = T;

    #[inline]
    fn poll(mut self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<T> {}
}
