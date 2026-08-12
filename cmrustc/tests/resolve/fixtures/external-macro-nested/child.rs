inherited!();

macro_rules! inherited {
    () => { struct ChildExpansion; }
}

inherited!();
mod grandchild;
