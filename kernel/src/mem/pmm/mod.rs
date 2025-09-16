use core::sync::atomic::AtomicUsize;

use crate::bindings::{
    self,
    error::{EResult, Errno},
};

pub mod phys_box;

/// Unsigned integer that can store a physical page number.
pub type AtomicPPN = AtomicUsize;
/// Unsigned integer that can store a physical page number.
pub type PPN = usize;

/// Allocate pages of physical memory.
pub unsafe fn page_alloc(count: usize) -> EResult<PPN> {
    let tmp = unsafe { bindings::raw::phys_page_alloc(count, false) };
    if tmp == 0 {
        return Err(Errno::ENOMEM);
    }
    Ok(tmp)
}

/// Free pages of physical memory.
pub unsafe fn page_free(ppn: PPN, count: usize) {
    // TODO: Current PMM doesn't support this sized model very well.
    unsafe { bindings::raw::phys_page_free(ppn) }
}
