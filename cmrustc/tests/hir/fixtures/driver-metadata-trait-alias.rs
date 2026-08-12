pub trait Pointee {
    type Metadata;
}

pub trait PointeeSized {}

pub trait Thin = Pointee<Metadata = ()> + PointeeSized;
