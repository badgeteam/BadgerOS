use super::{
    error::{EResult, Errno},
    raw::{self, sem_destroy, sem_t, timestamp_us_t},
};

pub struct Semaphore {
    inner: sem_t,
}

impl Semaphore {
    pub fn new() -> Self {
        unsafe {
            let mut inner: sem_t = core::mem::zeroed();
            raw::sem_init(&mut inner);
            Self { inner }
        }
    }
    pub fn post(&mut self) {
        unsafe {
            raw::sem_post(&raw mut self.inner);
        }
    }
    pub fn try_await(&mut self, timeout: timestamp_us_t) -> EResult<()> {
        unsafe { raw::sem_await(&raw mut self.inner, timeout) }
            .then_some(())
            .ok_or(Errno::ETIMEDOUT)
    }
}

impl Drop for Semaphore {
    fn drop(&mut self) {
        unsafe { sem_destroy(&raw mut self.inner) }
    }
}
