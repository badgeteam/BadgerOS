// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use crate::bindings::{self, irq, spinlock::Spinlock};

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default)]
/// Posix nanoseconds timestamp.
pub struct Timespec {
    /// Seconds (excluding leap) since 00:00, Jan 1 1970 UTC.
    pub sec: u64,
    /// Nanoseconds after [`Self::sec`].
    pub nsec: u32,
}

impl Timespec {
    pub fn now() -> Self {
        // TODO: Use actual RTC time instead of time since boot.
        let micros = unsafe { bindings::raw::time_us() } as u64;
        Self {
            sec: micros / 1000000,
            nsec: (micros % 1000000) as u32 * 1000,
        }
    }
}

/// Atomic version of [`Timespec`].
pub struct AtomicTimespec(Spinlock<Timespec>);

impl AtomicTimespec {
    pub fn new(time: Timespec) -> Self {
        Self(Spinlock::new(time))
    }

    pub fn load(&self) -> Timespec {
        let ie = unsafe { irq::disable() };
        let tmp = *self.0.lock_shared();
        unsafe { irq::enable_if(ie) };
        tmp
    }

    pub fn store(&self, value: Timespec) {
        let ie = unsafe { irq::disable() };
        *self.0.lock() = value;
        unsafe { irq::enable_if(ie) };
    }
}
