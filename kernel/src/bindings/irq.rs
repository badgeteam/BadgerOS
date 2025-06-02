use core::arch::asm;

use crate::bindings::raw::CSR_STATUS_IE_BIT;

/// Check whether interrupts are enabled.
#[cfg(any(target_arch = "riscv32", target_arch = "riscv64"))]
pub unsafe fn is_enabled() -> bool {
    let mut mask: usize;
    unsafe {
        asm!("csrr {tmp}, sstatus", tmp = out(reg) mask);
    }
    mask & (1 << CSR_STATUS_IE_BIT) != 0
}

/// Disable interrupts if some condition holds.
#[cfg(any(target_arch = "riscv32", target_arch = "riscv64"))]
pub unsafe fn disable_if(cond: bool) -> bool {
    let mut mask: usize = (cond as usize) << CSR_STATUS_IE_BIT;
    unsafe {
        asm!("csrrc {tmp}, sstatus, {tmp}", tmp = inout(reg) mask);
    }
    mask & (1 << CSR_STATUS_IE_BIT) != 0
}

/// Enable interrupts if some condition holds.
#[cfg(any(target_arch = "riscv32", target_arch = "riscv64"))]
pub unsafe fn enable_if(cond: bool) {
    let mask: usize = (cond as usize) << CSR_STATUS_IE_BIT;
    unsafe {
        asm!("csrs sstatus, {tmp}", tmp = in(reg) mask);
    }
}

/// Disable interrupts.
pub unsafe fn disable() -> bool {
    unsafe { disable_if(true) }
}

/// Enable interrupts.
pub unsafe fn enable() {
    unsafe { enable_if(true) }
}
