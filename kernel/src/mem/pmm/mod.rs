use core::sync::atomic::AtomicUsize;

use crate::bindings::{
    self,
    error::{EResult, Errno},
};

/// Unsigned integer that can store a physical page number.
pub type AtomicPPN = AtomicUsize;
/// Unsigned integer that can store a physical page number.
pub type PPN = usize;

/// Allocate a page of physical memory.
pub unsafe fn page_alloc(order: u8) -> EResult<PPN> {
    let tmp = unsafe { bindings::raw::phys_page_alloc(1usize << order, false) };
    if tmp == 0 {
        return Err(Errno::ENOMEM);
    }
    Ok(tmp)
}
