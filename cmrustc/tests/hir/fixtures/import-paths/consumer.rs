use crate::defs::{Direct as Alias, Generated as G};
use crate::defs as d;
use crate::{self as root};
use crate::defs::*;

#[cfg(off)]
use crate::missing::Never;

pub struct Consumer {
    direct: Direct,
    alias: Alias,
    module_alias: d::Public,
    reexport: crate::Facade,
    generated: G,
    root_alias: root::defs::Direct,
    chained: crate::Surface,
    self_alias: local_core::defs::Public,
}
