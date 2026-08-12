use crate::control_flow::ControlFlow;
use crate::ops::Try;

pub struct NeverShortCircuit<T>(pub T);
pub enum NeverShortCircuitResidual {}

impl<T> Try for NeverShortCircuit<T> {
    type Output = T;
    type Residual = NeverShortCircuitResidual;

    #[inline]
    fn branch(self) -> ControlFlow<NeverShortCircuitResidual, T> {}

    #[inline]
    fn from_output(x: T) -> Self {}
}
