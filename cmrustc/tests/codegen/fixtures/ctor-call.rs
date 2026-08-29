#![feature(no_core)]
#![no_core]
#![no_main]

// std's `impl FromRawFd for FileDesc { fn from_raw_fd(fd) -> Self {
// Self(OwnedFd::from_raw_fd(fd)) } }`: a tuple-struct constructor
// called through `Self` with an associated-fn argument.
mod inner {
    pub struct OwnedFd {
        pub fd: i32,
    }
    impl OwnedFd {
        pub fn from_raw_fd(fd: i32) -> OwnedFd { OwnedFd { fd } }
    }
}

use inner::OwnedFd;

pub struct FileDesc(OwnedFd);

impl FileDesc {
    fn from_raw_fd(fd: i32) -> Self {
        Self(OwnedFd::from_raw_fd(fd))
    }
    fn via_name(fd: i32) -> FileDesc {
        FileDesc(OwnedFd::from_raw_fd(fd + 10))
    }
    fn raw(&self) -> i32 { self.0.fd }
}

#[no_mangle]
pub extern "C" fn ctor_call(x: i32) -> i32 {
    FileDesc::from_raw_fd(x).raw() * 100 + FileDesc::via_name(x).raw()
}
