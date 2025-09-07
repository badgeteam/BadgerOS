use core::arch::asm;

#[cfg(any(target_arch = "riscv32", target_arch = "riscv64"))]
use crate::bindings::raw::CSR_STATUS_IE_BIT;

/// Check whether interrupts are enabled.
#[cfg(any(target_arch = "riscv32", target_arch = "riscv64"))]
#[inline(always)]
pub unsafe fn is_enabled() -> bool {
    let mut mask: usize;
    unsafe { asm!("csrr {tmp}, sstatus", tmp = out(reg) mask) };
    mask & (1 << CSR_STATUS_IE_BIT) != 0
}

/// Disable interrupts if some condition holds.
#[cfg(any(target_arch = "riscv32", target_arch = "riscv64"))]
#[inline(always)]
pub unsafe fn disable_if(cond: bool) -> bool {
    let mut mask: usize = (cond as usize) << CSR_STATUS_IE_BIT;
    unsafe { asm!("csrrc {tmp}, sstatus, {tmp}", tmp = inout(reg) mask) };
    mask & (1 << CSR_STATUS_IE_BIT) != 0
}

/// Enable interrupts if some condition holds.
#[cfg(any(target_arch = "riscv32", target_arch = "riscv64"))]
#[inline(always)]
pub unsafe fn enable_if(cond: bool) {
    let mask: usize = (cond as usize) << CSR_STATUS_IE_BIT;
    unsafe { asm!("csrs sstatus, {tmp}", tmp = in(reg) mask) };
}

/// Check whether interrupts are enabled.
#[cfg(any(target_arch = "x86_64"))]
#[inline(always)]
pub unsafe fn is_enabled() -> bool {
    let tmp: usize;
    // Pure and readonly options mean this will be eliminated if the result is unused.
    unsafe { asm!("pushf; pop {tmp}", tmp = out(reg) tmp, options(pure, readonly)) };
    tmp & (1 << 9) != 0
}

/// Disable interrupts if some condition holds.
#[cfg(any(target_arch = "x86_64"))]
#[inline(always)]
pub unsafe fn disable_if(cond: bool) -> bool {
    let enabled = unsafe { is_enabled() };
    if cond {
        unsafe { asm!("cli") };
    }
    enabled
}

/// Enable interrupts if some condition holds.
#[cfg(any(target_arch = "x86_64"))]
#[inline(always)]
pub unsafe fn enable_if(cond: bool) {
    if cond {
        unsafe { asm!("sti") };
    }
}

/// Disable interrupts.
#[inline(always)]
pub unsafe fn disable() -> bool {
    unsafe { disable_if(true) }
}

/// Enable interrupts.
#[inline(always)]
pub unsafe fn enable() {
    unsafe { enable_if(true) }
}

/// Guard that disable interrupts momentarily.
pub struct IrqGuard {
    was_enabled: bool,
}

impl IrqGuard {
    pub unsafe fn new() -> Self {
        IrqGuard {
            was_enabled: unsafe { disable() },
        }
    }
}

impl Drop for IrqGuard {
    fn drop(&mut self) {
        unsafe { enable_if(self.was_enabled) };
    }
}
