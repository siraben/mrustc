use crate::Facade as Imported;

pub struct Consumer {
    value: Imported,
    overridden: Imported<u16>,
}
