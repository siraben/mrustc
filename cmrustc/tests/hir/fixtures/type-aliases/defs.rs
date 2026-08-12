pub struct Marker;

pub type Chain<T = Marker> = Pair<T>;
pub type Pair<T> = (T, *const T);
