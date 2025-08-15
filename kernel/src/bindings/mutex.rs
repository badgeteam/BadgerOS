use core::{
    cell::UnsafeCell,
    marker::PhantomData,
    mem::MaybeUninit,
    ops::{Deref, DerefMut},
};

use super::{
    error::{EResult, Errno},
    raw::{self, mutex_t, spinlock_t, timestamp_us_t},
};

/// A BadgerOS mutex with a value in it.
#[repr(C)]
pub struct Mutex<T, const SHARED: bool = true> {
    inner: UnsafeCell<mutex_t>,
    data: UnsafeCell<T>,
}
unsafe impl<T, const SHARED: bool> Send for Mutex<T, SHARED> {}
unsafe impl<T, const SHARED: bool> Sync for Mutex<T, SHARED> {}

impl<T, const SHARED: bool> Mutex<T, SHARED> {
    pub unsafe fn mutex(&self) -> &mut mutex_t {
        unsafe { self.inner.as_mut_unchecked() }
    }

    pub unsafe fn data(&self) -> &mut T {
        unsafe { self.data.as_mut_unchecked() }
    }

    /// Create a new mutex statically; you must only use this to initialize the `.data` section.
    pub const unsafe fn new_static(data: T) -> Self {
        Self {
            inner: UnsafeCell::new(mutex_t {
                is_shared: SHARED,
                shares: 0,
                waiting_list: unsafe { core::mem::zeroed() },
            }),
            data: UnsafeCell::new(data),
        }
    }

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
    pub fn try_lock<'a>(&'a self, timeout: timestamp_us_t) -> EResult<MutexGuard<'a, T, SHARED>> {
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
    ) -> EResult<SharedMutexGuard<'a, T>> {
        SharedMutexGuard::try_new(self, timeout)
    }

    /// Lock the mutex as shared.
    pub fn lock_shared<'a>(&'a self) -> SharedMutexGuard<'a, T> {
        SharedMutexGuard::new(self)
    }
}

impl<T, const SHARED: bool> Mutex<T, SHARED> {
    /// Write the value in the mutex.
    pub fn write(&self, value: T) {
        *self.lock() = value
    }
}

impl<T: Clone> Mutex<T, true> {
    /// Read the value in the mutex.
    pub fn read(&self) -> T {
        self.lock_shared().clone()
    }
}

impl<T: Clone> Mutex<T, false> {
    /// Read the value in the mutex.
    pub fn read(&self) -> T {
        self.lock().clone()
    }
}

/// Represents a locked mutex.
pub struct MutexGuard<'a, T, const SHARED: bool> {
    mutex: *mut mutex_t,
    data: &'a mut T,
    marker: PhantomData<&'a Mutex<T, SHARED>>,
}

impl<'a, T, const SHARED: bool> MutexGuard<'a, T, SHARED> {
    /// Try to lock a mutex.
    pub unsafe fn try_new_raw(
        mutex: *mut mutex_t,
        data: &'a mut T,
        timeout: timestamp_us_t,
    ) -> EResult<Self> {
        unsafe {
            raw::mutex_acquire(mutex, timeout)
                .then_some(Self {
                    mutex,
                    data,
                    marker: PhantomData {},
                })
                .ok_or(Errno::ETIMEDOUT)
        }
    }
    /// Lock a mutex.
    pub unsafe fn new_raw(mutex: *mut mutex_t, data: &'a mut T) -> Self {
        unsafe { Self::try_new_raw(mutex, data, timestamp_us_t::MAX) }.unwrap()
    }
    /// Try to lock a mutex.
    pub fn try_new(mutex: &'a Mutex<T, SHARED>, timeout: timestamp_us_t) -> EResult<Self> {
        unsafe {
            raw::mutex_acquire(mutex.mutex(), timeout)
                .then_some(Self {
                    mutex: mutex.mutex(),
                    data: mutex.data(),
                    marker: PhantomData {},
                })
                .ok_or(Errno::ETIMEDOUT)
        }
    }
    /// Lock a mutex.
    pub fn new(mutex: &'a Mutex<T, SHARED>) -> Self {
        Self::try_new(mutex, timestamp_us_t::MAX).unwrap()
    }
}

impl<'a, T, const SHARED: bool> Drop for MutexGuard<'a, T, SHARED> {
    fn drop(&mut self) {
        unsafe { raw::mutex_release(self.mutex) };
    }
}

impl<T, const SHARED: bool> Deref for MutexGuard<'_, T, SHARED> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.data
    }
}

impl<T, const SHARED: bool> DerefMut for MutexGuard<'_, T, SHARED> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.data
    }
}

/// Represents a mutex locked shared.
pub struct SharedMutexGuard<'a, T> {
    mutex: *mut mutex_t,
    data: &'a T,
    marker: PhantomData<&'a Mutex<T, true>>,
}

impl<'a, T> SharedMutexGuard<'a, T> {
    /// Try to lock a mutex as shared.
    pub unsafe fn try_new_raw(
        mutex: *mut mutex_t,
        data: &'a T,
        timeout: timestamp_us_t,
    ) -> EResult<Self> {
        unsafe {
            raw::mutex_acquire_shared(mutex, timeout)
                .then_some(Self {
                    mutex,
                    data,
                    marker: PhantomData {},
                })
                .ok_or(Errno::ETIMEDOUT)
        }
    }
    /// Lock a mutex as shared.
    pub unsafe fn new_raw(mutex: *mut mutex_t, data: &'a T) -> Self {
        unsafe { Self::try_new_raw(mutex, data, timestamp_us_t::MAX) }.unwrap()
    }
    /// Try to lock a mutex as shared.
    pub fn try_new(mutex: &'a Mutex<T, true>, timeout: timestamp_us_t) -> EResult<Self> {
        unsafe {
            raw::mutex_acquire_shared(mutex.mutex(), timeout)
                .then_some(Self {
                    mutex: mutex.mutex(),
                    data: mutex.data(),
                    marker: PhantomData {},
                })
                .ok_or(Errno::ETIMEDOUT)
        }
    }
    /// Lock a mutex as shared.
    pub fn new(mutex: &'a Mutex<T, true>) -> Self {
        Self::try_new(mutex, timestamp_us_t::MAX).unwrap()
    }
}

impl<'a, T> Drop for SharedMutexGuard<'a, T> {
    fn drop(&mut self) {
        unsafe { raw::mutex_release_shared(self.mutex) };
    }
}

impl<'a, T> Deref for SharedMutexGuard<'a, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.data
    }
}
