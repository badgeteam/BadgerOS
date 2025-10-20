// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{
    cell::UnsafeCell,
    sync::atomic::{AtomicUsize, Ordering},
};

use alloc::boxed::Box;

use crate::bindings::error::EResult;

/// FIFO data buffer.
pub struct Fifo {
    /// Data buffer.
    data: Box<UnsafeCell<[u8]>>,
    /// Read reserved position.
    read_resv: AtomicUsize,
    /// Read commit position.
    read_commit: AtomicUsize,
    /// Write reserved position.
    write_resv: AtomicUsize,
    /// Write commit position.
    write_commit: AtomicUsize,
}
unsafe impl Sync for Fifo {}

impl Fifo {
    /// Create a new FIFO data buffer.
    pub fn new() -> EResult<Self> {
        Ok(Self {
            data: Box::try_new(UnsafeCell::new([0; 8192]))?,
            read_resv: AtomicUsize::new(0),
            read_commit: AtomicUsize::new(0),
            write_resv: AtomicUsize::new(0),
            write_commit: AtomicUsize::new(0),
        })
    }

    /// Read data from the buffer.
    pub fn read(&self, rdata: &mut [u8]) -> usize {
        let ptr = unsafe { self.data.as_ref_unchecked() };
        let cap = ptr.len();

        // Try to reserve data.
        let mut rx = self.read_resv.load(Ordering::Relaxed);
        let tx = self.write_commit.load(Ordering::Relaxed);
        let mut recv_cap;

        loop {
            recv_cap = (tx.wrapping_sub(rx).wrapping_add(cap) % cap).min(rdata.len());
            if let Err(x) = self.read_resv.compare_exchange(
                rx,
                rx.wrapping_add(recv_cap) % cap,
                Ordering::Acquire,
                Ordering::Relaxed,
            ) {
                rx = x;
            } else {
                break;
            }
        }
        if recv_cap == 0 {
            return 0;
        }

        // Copy data out of the FIFO's buffer.
        let start_off = rx;
        let end_off = rx.wrapping_add(recv_cap) % cap;
        if end_off > start_off {
            for i in start_off..end_off {
                rdata[i - start_off] = ptr[i];
            }
        } else {
            for i in start_off..cap {
                rdata[i - start_off] = ptr[i];
            }
            for i in 0..end_off {
                rdata[i - (cap - start_off)] = ptr[i];
            }
        }

        // Mark the read as completed.
        while let Err(_) = self.read_commit.compare_exchange(
            start_off,
            end_off,
            Ordering::Relaxed,
            Ordering::Relaxed,
        ) {}

        recv_cap
    }

    /// Write data to the buffer.
    pub fn write(&self, wdata: &[u8]) -> usize {
        let ptr = unsafe { self.data.as_mut_unchecked() };
        let cap = ptr.len();

        // Try to reserve space.
        let rx = self.read_commit.load(Ordering::Relaxed);
        let mut tx = self.write_resv.load(Ordering::Relaxed);
        let mut send_cap: usize;

        loop {
            send_cap =
                (rx.wrapping_sub(rx).wrapping_add(cap).wrapping_sub(1) % cap).min(wdata.len());
            if let Err(x) = self.write_resv.compare_exchange(
                tx,
                tx.wrapping_add(send_cap) % cap,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                tx = x;
            } else {
                break;
            }
        }
        if send_cap == 0 {
            return 0;
        }

        // Copy the data into the FIFO's buffer.
        let start_off = tx;
        let end_off = start_off.wrapping_add(send_cap) % cap;
        if end_off > start_off {
            for i in start_off..end_off {
                ptr[i] = wdata[i - start_off];
            }
        } else {
            for i in start_off..cap {
                ptr[i] = wdata[i - start_off];
            }
            for i in 0..end_off {
                ptr[i] = wdata[i - (cap - start_off)];
            }
        }

        // Mark the write as completed.
        while let Err(_) = self.write_commit.compare_exchange(
            start_off,
            end_off,
            Ordering::Release,
            Ordering::Relaxed,
        ) {}

        send_cap
    }

    /// Get the amount of available read data.
    pub fn read_avl(&self) -> usize {
        let cap = unsafe { self.data.as_ref_unchecked() }.len();
        let rx = self.read_resv.load(Ordering::Relaxed);
        let tx = self.write_commit.load(Ordering::Relaxed);
        tx.wrapping_sub(rx).wrapping_add(cap) % cap
    }

    /// Get the amount of available write space.
    pub fn write_avl(&self) -> usize {
        let cap = unsafe { self.data.as_ref_unchecked() }.len();
        let rx = self.read_resv.load(Ordering::Relaxed);
        let tx = self.write_commit.load(Ordering::Relaxed);
        rx.wrapping_sub(tx).wrapping_add(cap).wrapping_sub(1) % cap
    }
}
