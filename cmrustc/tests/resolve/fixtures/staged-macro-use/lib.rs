macro_rules! emit_from_child {
    () => { struct WrongBeforeMacroUse; };
}

#[macro_use]
mod definitions;

emit_from_child!();

macro_rules! wrap_child {
    () => { generated_from_child!(); };
}

wrap_child!();

macro_rules! emit_from_child {
    () => { struct FromRootShadow; };
}

emit_from_child!();
