use core::{ffi::c_void, marker::PhantomData, mem::MaybeUninit};

use alloc::vec::Vec;

use super::{
    error::{Signal, SignalError},
    raw::{self, process_t},
};

/// Helper for safely accessing usermode-owned structs.
pub struct UserPtr<T: Copy, const WRITE: bool> {
    inner: UserArr<T, WRITE>,
}

impl<T: Copy, const WRITE: bool> UserPtr<T, WRITE> {
    pub unsafe fn new(process: *mut process_t, vaddr: usize) -> Self {
        Self {
            inner: UserArr {
                process: process,
                vaddr: vaddr,
                len: 1,
                marker: PhantomData,
            },
        }
    }
    pub fn read(&self) -> Result<T, SignalError> {
        self.inner.read(0)
    }
}

impl<T: Copy> UserPtr<T, true> {
    pub fn write(&self, data: &T) -> Result<(), SignalError> {
        self.inner.write(0, data)
    }
}

/// Helper for safely accessing usermode-owned arrays.
pub struct UserArr<T: Copy, const WRITE: bool> {
    process: *mut process_t,
    vaddr: usize,
    len: usize,
    marker: PhantomData<T>,
}

impl<T: Copy, const WRITE: bool> UserArr<T, WRITE> {
    pub unsafe fn new(process: *mut process_t, vaddr: usize, len: usize) -> Self {
        Self {
            process,
            vaddr,
            len,
            marker: PhantomData,
        }
    }
    /// Read a single element.
    pub fn read(&self, index: usize) -> Result<T, SignalError> {
        if index >= self.len {
            panic!(
                "UserArr index out of bounds: {} in array of len {}",
                index, self.len
            );
        }
        unsafe {
            let mut data: MaybeUninit<T> = MaybeUninit::uninit();
            raw::copy_from_user_raw(
                self.process,
                data.as_mut_ptr() as *mut c_void,
                self.vaddr + index * size_of::<T>(),
                size_of::<T>(),
            )
            .then(|| data.assume_init())
            .ok_or(SignalError {
                signal: Signal::SIGSEGV,
                cause: self.vaddr + index * size_of::<T>(),
            })
        }
    }
    /// Read a range of elements into an existing array.
    pub fn read_range_into(&self, index: usize, rdata: &mut [T]) -> Result<(), SignalError> {
        if index + rdata.len() >= self.len {
            panic!(
                "UserArr index out of bounds: {} len {} in array of len {}",
                index,
                rdata.len(),
                self.len
            );
        }
        unsafe {
            raw::copy_from_user_raw(
                self.process,
                rdata.as_mut_ptr() as *mut c_void,
                self.vaddr + index * size_of::<T>(),
                rdata.len() * size_of::<T>(),
            )
            .then_some(())
            .ok_or(SignalError {
                signal: Signal::SIGSEGV,
                cause: self.vaddr + index * size_of::<T>(),
            })
        }
    }
    /// Read a range of elements into a heap-allocated array.
    pub fn read_range(&self, index: usize, len: usize) -> Result<Vec<T>, SignalError> {
        let maybe_self: UserArr<MaybeUninit<T>, false> = UserArr {
            process: self.process,
            vaddr: self.vaddr,
            len: self.len,
            marker: PhantomData,
        };
        let mut tmp = Vec::new();
        tmp.resize(len, MaybeUninit::uninit());
        maybe_self
            .read_range_into(index, &mut tmp)
            .map(|_| unsafe { core::mem::transmute(tmp) })
    }
}

impl<T: Copy> UserArr<T, true> {
    /// Write a single element.
    pub fn write(&self, index: usize, data: &T) -> Result<(), SignalError> {
        if index >= self.len {
            panic!(
                "UserArr index out of bounds: {} in array of len {}",
                index, self.len
            );
        }
        unsafe {
            raw::copy_to_user_raw(
                self.process,
                self.vaddr + index * size_of::<T>(),
                data as *const T as *const c_void,
                size_of::<T>(),
            )
            .then_some(())
            .ok_or(SignalError {
                signal: Signal::SIGSEGV,
                cause: self.vaddr + index * size_of::<T>(),
            })
        }
    }
    /// Write a range of elements.
    pub fn write_range(&self, index: usize, data: &[T]) -> Result<(), SignalError> {
        if index + data.len() >= self.len {
            panic!(
                "UserArr index out of bounds: {} len {} in array of len {}",
                index,
                data.len(),
                self.len
            );
        }
        unsafe {
            raw::copy_to_user_raw(
                self.process,
                self.vaddr + index * size_of::<T>(),
                data.as_ptr() as *const c_void,
                size_of::<T>() * data.len(),
            )
            .then_some(())
            .ok_or(SignalError {
                signal: Signal::SIGSEGV,
                cause: self.vaddr + index * size_of::<T>(),
            })
        }
    }
}
