use crate::convert;
use crate::ops;

pub enum ControlFlow<B, C> {
    Continue(C),
    Break(B),
}

#[unstable(feature = "try_trait_v2", issue = "84277", old_name = "try_trait")]
impl<B, C> ops::Try for ControlFlow<B, C> {
    type Output = C;
    type Residual = ControlFlow<B, convert::Infallible>;

    #[inline]
    fn from_output(output: Self::Output) -> Self {}

    #[inline]
    fn branch(self) -> ControlFlow<Self::Residual, Self::Output> {}
}
