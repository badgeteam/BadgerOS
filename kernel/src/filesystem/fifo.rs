// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{
    cell::UnsafeCell,
    sync::atomic::{AtomicU32, AtomicUsize, Ordering, fence},
};

use alloc::{boxed::Box, sync::Arc};

use crate::bindings::{
    error::{EResult, Errno},
    irq,
    raw::timestamp_us_t,
    spinlock::Spinlock,
    thread::Waitlist,
};

use super::{File, SeekMode, Stat, VNode};

/// FIFO data buffer.
struct FifoBuffer {
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
unsafe impl Sync for FifoBuffer {}

impl FifoBuffer {
    /// Create a new FIFO data buffer.
    pub fn new() -> Self {
        Self {
            data: Box::new(UnsafeCell::new([0; 8192])),
            read_resv: AtomicUsize::new(0),
            read_commit: AtomicUsize::new(0),
            write_resv: AtomicUsize::new(0),
            write_commit: AtomicUsize::new(0),
        }
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

/// Data shared between all FIFO handles, regardless of whether it has a vnode.
pub(super) struct FifoShared {
    /// FIFO data storage.
    buffer: Spinlock<Option<Box<FifoBuffer>>>,
    /// Number of readers.
    read_count: AtomicU32,
    /// Number of writers.
    write_count: AtomicU32,
    /// Waiting list for readers.
    read_queue: Waitlist,
    /// Waiting list for writers.
    write_queue: Waitlist,
}

impl FifoShared {
    /// Create new shared FIFO data.
    pub(super) fn new() -> Arc<Self> {
        let tmp = Arc::new(Self {
            buffer: Spinlock::new(None),
            read_count: AtomicU32::new(0),
            write_count: AtomicU32::new(0),
            read_queue: Waitlist::new(),
            write_queue: Waitlist::new(),
        });
        fence(Ordering::Release);
        tmp
    }

    /// Handle a file open on a FIFO.
    pub(super) fn open(&self, nonblock: bool, is_read: bool, is_write: bool) {
        assert!(is_read || is_write);
        assert!(unsafe { irq::disable() });
        let nonblock = nonblock || (is_read && is_write);

        if is_read {
            self.read_count.fetch_add(1, Ordering::Relaxed);
        }
        if is_write {
            self.write_count.fetch_add(1, Ordering::Relaxed);
        }

        let mut guard = if nonblock {
            self.buffer.lock()
        } else {
            let queue = if is_read {
                &self.read_queue
            } else {
                &self.write_queue
            };
            let mut taken_lock = None;
            let taken_lock_ptr = &mut taken_lock;

            queue.block(timestamp_us_t::MAX, &mut move || {
                *taken_lock_ptr = Some(self.buffer.lock());

                // Unblock if the other end is open.
                if is_read && self.write_count.load(Ordering::Acquire) != 0 {
                    return true;
                } else if !is_read && self.read_count.load(Ordering::Acquire) != 0 {
                    return true;
                };

                *taken_lock_ptr = None;
                false
            });

            // Take the spinlock had it not been taken already.
            taken_lock.unwrap_or_else(|| self.buffer.lock())
        };

        // If both readers and writers exist, and there is no buffer, create it.
        if self.read_count.load(Ordering::Relaxed) != 0
            && self.write_count.load(Ordering::Relaxed) != 0
            && guard.is_none()
        {
            *guard = Some(Box::new(FifoBuffer::new()));
        }
        drop(guard);

        // Wake writers on the fifo.
        self.write_queue.notify();

        unsafe { irq::enable() };
    }

    /// Handle a file close on the FIFO.
    fn close(&self, had_read: bool, had_write: bool) {
        // TODO: Destroy buffer if all readers or all writers close.
        if had_read {
            self.read_count.fetch_sub(1, Ordering::Relaxed);
        }
        if had_write {
            self.write_count.fetch_sub(1, Ordering::Relaxed);
        }
    }

    /// Handle a file read for a FIFO.
    /// WARNING: May sporadically return 0 in a blocking multi-read scenario.
    fn read(&self, nonblock: bool, rdata: &mut [u8]) -> EResult<usize> {
        let _noirq = unsafe { irq::IrqGuard::new() };

        let buffer = if nonblock {
            self.buffer.lock_shared()
        } else {
            let mut buffer = None;
            let buffer_ptr = &mut buffer;
            self.read_queue.block(timestamp_us_t::MAX, &mut || {
                // Unblock if there is read data available.
                *buffer_ptr = Some(self.buffer.lock_shared());
                if let Some(buffer) = &**buffer_ptr.as_ref().unwrap()
                    && buffer.read_avl() > 0
                {
                    return false;
                }
                *buffer_ptr = None;
                true
            });
            buffer.unwrap_or_else(|| self.buffer.lock_shared())
        };

        let mut res = try { buffer.as_ref().ok_or(Errno::EAGAIN)?.read(rdata) };
        if res == Ok(0) && nonblock {
            res = Err(Errno::EAGAIN)
        };

        // Wake blocking writers.
        self.write_queue.notify();

        res
    }

    /// Handle a file write for a FIFO.
    /// Raises EPIPE if `enforce_open` is true and the read end is closed.
    fn write(&self, nonblock: bool, wdata: &[u8], enforce_open: bool) -> EResult<usize> {
        let _noirq = unsafe { irq::IrqGuard::new() };
        if enforce_open && self.read_count.load(Ordering::Relaxed) == 0 {
            return Err(Errno::EPIPE);
        }

        let buffer = if nonblock {
            self.buffer.lock_shared()
        } else {
            let mut buffer = None;
            let buffer_ptr = &mut buffer;
            self.write_queue.block(timestamp_us_t::MAX, &mut || {
                // Unblock if there is write data available.
                *buffer_ptr = Some(self.buffer.lock_shared());
                if let Some(buffer) = &**buffer_ptr.as_ref().unwrap()
                    && buffer.write_avl() > 0
                {
                    return false;
                }
                *buffer_ptr = None;
                true
            });
            buffer.unwrap_or_else(|| self.buffer.lock_shared())
        };

        let mut res = try { buffer.as_ref().ok_or(Errno::EAGAIN)?.write(wdata) };
        if res == Ok(0) && nonblock {
            res = Err(Errno::EAGAIN)
        };

        // Wake blocking readers.
        self.read_queue.notify();

        res
    }
}
unsafe impl Send for FifoShared {}
unsafe impl Sync for FifoShared {}

/// A FIFO or a pipe file descriptor.
pub struct Fifo {
    /// VNode, if any.
    pub(super) vnode: Option<Arc<VNode>>,
    /// Access is non-blocking.
    pub(super) is_nonblock: bool,
    /// This handle allows reading.
    pub(super) allow_read: bool,
    /// This handle allows writing.
    pub(super) allow_write: bool,
    /// Handle to the FIFO data buffer.
    pub(super) shared: Arc<FifoShared>,
}

impl Drop for Fifo {
    fn drop(&mut self) {
        self.shared.close(self.allow_read, self.allow_write);
    }
}

impl File for Fifo {
    fn stat(&self) -> EResult<Stat> {
        if let Some(vnode) = &self.vnode {
            vnode.ops.lock_shared().stat(&vnode)
        } else {
            Ok(Stat::default())
        }
    }

    fn tell(&self) -> EResult<u64> {
        Err(Errno::ESPIPE)
    }

    fn seek(&self, _mode: SeekMode, _offset: i64) -> EResult<u64> {
        Err(Errno::ESPIPE)
    }

    fn write(&self, mut wdata: &[u8]) -> EResult<usize> {
        let mut wlen = self.shared.write(self.is_nonblock, wdata, false)?;
        {
            let range = wlen..wdata.len();
            wdata = &wdata[range];
        }
        while wdata.len() > 0 {
            match self.shared.write(self.is_nonblock, wdata, true) {
                Ok(res) => {
                    wlen += res;
                    let range = res..wdata.len();
                    wdata = &wdata[range];
                }
                Err(Errno::EPIPE) => return Err(Errno::EPIPE),
                Err(_) => break,
            }
        }
        Ok(wlen)
    }

    fn read(&self, mut rdata: &mut [u8]) -> EResult<usize> {
        let mut rlen = self.shared.read(self.is_nonblock, rdata)?;
        {
            let range = rlen..rdata.len();
            rdata = &mut rdata[range];
        }
        while rdata.len() > 0
            && let Ok(res) = self.shared.read(true, rdata)
        {
            rlen += res;
            let range = res..rdata.len();
            rdata = &mut rdata[range];
        }
        Ok(rlen)
    }

    fn resize(&self, _size: u64) -> EResult<()> {
        Err(Errno::ESPIPE)
    }

    fn sync(&self) -> EResult<()> {
        Err(Errno::ESPIPE)
    }

    fn get_vnode(&self) -> Option<Arc<VNode>> {
        self.vnode.clone()
    }
}
