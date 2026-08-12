#[rustc_specialization_trait]
pub(crate) unsafe trait AsVecIntoIter {
    type Item;
    fn as_into_iter(&mut self) -> &mut super::IntoIter<Self::Item>;
}
