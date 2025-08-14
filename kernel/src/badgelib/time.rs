// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use crate::bindings::{self, irq, spinlock::Spinlock};

#[repr(C)]
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

/// Get the number of days in a month (0-11) of a certain year.
pub const fn days_in_month(of_year: u16, month: u8) -> u8 {
    let is_leap = of_year % 4 == 0 && (of_year % 100 != 0 || of_year % 400 == 0);
    match month {
        0 => 31,
        1 => 28 + is_leap as u8,
        2 => 31,
        3 => 30,
        4 => 31,
        5 => 30,
        6 => 31,
        7 => 31,
        8 => 30,
        9 => 31,
        10 => 30,
        11 => 31,
        _ => unreachable!(),
    }
}
