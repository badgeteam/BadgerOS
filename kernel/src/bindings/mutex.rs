use core::{
    cell::UnsafeCell,
    mem::MaybeUninit,
    ops::{Deref, DerefMut},
};

use super::raw::{self, mutex_t, timestamp_us_t};

/// A BadgerOS mutex.
pub struct Mutex<T, const SHARED: bool> {
    inner: UnsafeCell<mutex_t>,
    data: UnsafeCell<T>,
}
unsafe impl<T, const SHARED: bool> Send for Mutex<T, SHARED> {}
unsafe impl<T, const SHARED: bool> Sync for Mutex<T, SHARED> {}

impl<T, const SHARED: bool> Mutex<T, SHARED> {
    /// Create a new mutex.
    pub fn new(data: T) -> Self {
        let mut inner: mutex_t = unsafe { MaybeUninit::zeroed().assume_init() };
        unsafe { raw::mutex_init(&mut inner, SHARED) };
        Self {
            inner: UnsafeCell::new(inner),
            data: UnsafeCell::new(data),
        }
    }
    /// Try to lock the mutex.
    pub fn try_lock<'a>(&'a self, timeout: timestamp_us_t) -> Option<MutexGuard<'a, T, SHARED>> {
        MutexGuard::try_new(self, timeout)
    }
    /// Lock the mutex.
    pub fn lock<'a>(&'a self) -> MutexGuard<'a, T, SHARED> {
        MutexGuard::new(self)
    }
}

impl<T> Mutex<T, true> {
    /// Try to lock the mutex as shared.
    pub fn try_lock_shared<'a>(
        &'a self,
        timeout: timestamp_us_t,
    ) -> Option<SharedMutexGuard<'a, T>> {
        SharedMutexGuard::try_new(self, timeout)
    }
    /// Lock the mutex as shared.
    pub fn lock_shared<'a>(&'a self) -> SharedMutexGuard<'a, T> {
        SharedMutexGuard::new(self)
    }
}

/// Represents a locked mutex.
pub struct MutexGuard<'a, T, const SHARED: bool> {
    mutex: &'a Mutex<T, SHARED>,
}

impl<'a, T, const SHARED: bool> MutexGuard<'a, T, SHARED> {
    /// Try to lock a mutex.
    pub fn try_new(mutex: &'a Mutex<T, SHARED>, timeout: timestamp_us_t) -> Option<Self> {
        unsafe {
            raw::mutex_acquire(mutex.inner.as_mut_unchecked(), timeout).then_some(Self { mutex })
        }
    }
    /// Lock a mutex.
    pub fn new(mutex: &'a Mutex<T, SHARED>) -> Self {
        Self::try_new(mutex, timestamp_us_t::MAX).unwrap()
    }
}

impl<'a, T, const SHARED: bool> Drop for MutexGuard<'a, T, SHARED> {
    fn drop(&mut self) {
        unsafe {
            raw::mutex_release(self.mutex.inner.as_mut_unchecked());
        }
    }
}

impl<T, const SHARED: bool> Deref for MutexGuard<'_, T, SHARED> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        unsafe { self.mutex.data.as_ref_unchecked() }
    }
}

impl<T, const SHARED: bool> DerefMut for MutexGuard<'_, T, SHARED> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        unsafe { self.mutex.data.as_mut_unchecked() }
    }
}

/// Represents a mutex locked shared.
pub struct SharedMutexGuard<'a, T> {
    mutex: &'a Mutex<T, true>,
}

impl<'a, T> SharedMutexGuard<'a, T> {
    /// Try to lock a mutex as shared.
    pub fn try_new(mutex: &'a Mutex<T, true>, timeout: timestamp_us_t) -> Option<Self> {
        unsafe {
            raw::mutex_acquire_shared(mutex.inner.as_mut_unchecked(), timeout)
                .then_some(Self { mutex })
        }
    }
    /// Lock a mutex as shared.
    pub fn new(mutex: &'a Mutex<T, true>) -> Self {
        Self::try_new(mutex, timestamp_us_t::MAX).unwrap()
    }
}

impl<'a, T> Deref for SharedMutexGuard<'a, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        unsafe { self.mutex.data.as_ref_unchecked() }
    }
}
