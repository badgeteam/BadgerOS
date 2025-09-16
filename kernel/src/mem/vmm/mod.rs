// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::{
    ops::Div,
    ptr::{slice_from_raw_parts, slice_from_raw_parts_mut},
    sync::atomic::AtomicUsize,
};

use crate::{
    bindings::{
        self,
        error::{EResult, Errno},
        mutex::{Mutex, SharedMutexGuard},
    },
    config::PAGE_SIZE,
    cpu::{
        self,
        mmu::{BITS_PER_LEVEL, PackedPTE},
    },
    mem::{pmm::PPN, vmm::mmu::PAGING_LEVELS},
};

use super::pmm;

pub mod mmu;

#[rustfmt::skip]
pub mod flags {
    // Note: These flags are the same bit positions as in the RISC-V PTE format, do not change them!
    
    /// Map memory as executable.
    pub const R:   u32 = 0b0000_0000_0010;
    /// Map memory as writeable (reads must also be allowed).
    pub const W:   u32 = 0b0000_0000_0100;
    /// Map memory as executable.
    pub const X:   u32 = 0b0000_0000_1000;
    /// Map memory as user-accessible.
    pub const U:   u32 = 0b0000_0001_0000;
    /// Map memory as global (exists in all page ASIDs).
    pub const G:   u32 = 0b0000_0010_0000;
    /// Page was accessed since this flag was last cleared.
    pub const A:   u32 = 0b0000_0100_0000;
    /// Page was written since this flag was last cleared.
    pub const D:   u32 = 0b0000_1000_0000;
    
    /// Mark page as copy-on-write (W must be disabled).
    pub const COW: u32 = 0b0001_0000_0000;
    
    /// Map memory as I/O (uncached, no write coalescing).
    pub const IO:  u32 = 0b0100_0000_0000;
    /// Map memory as uncached write coalescing.
    pub const NC:  u32 = 0b1000_0000_0000;
    
    /// Map memory as read-write.
    pub const RW:  u32 = R | W;
    /// Map memory as read-execute.
    pub const RX:  u32 = R | X;
    /// Map memory as read-write-execute.
    pub const RWX: u32 = R | W | X;
}

/// Unsigned integer that can store a virtual page number.
pub type AtomicVPN = AtomicUsize;
/// Unsigned integer that can store a virtual page number.
pub type VPN = usize;

/// Kernel page table root PPN.
pub static KERNEL_PAGE_TABLE: Mutex<PPN> = unsafe { Mutex::new_static(0) };

unsafe extern "C" {
    static __start_text: [u8; 0];
    static __stop_text: [u8; 0];
    static __start_rodata: [u8; 0];
    static __stop_rodata: [u8; 0];
    static __start_data: [u8; 0];
    static __stop_data: [u8; 0];

    /// Higher-half direct map virtual address.
    /// Provided by boot protocol.
    #[link_name = "rust_hhdm_vaddr"]
    static mut HHDM_VADDR: usize;
    /// Higher-half direct map address offset (paddr -> vaddr).
    /// Provided by boot protocol.
    #[link_name = "rust_hhdm_offset"]
    static mut HHDM_OFFSET: usize;
    /// Higher-half direct map size.
    /// Provided by boot protocol.
    #[link_name = "rust_hhdm_size"]
    static mut HHDM_SIZE: usize;
    /// Kernel base virtual address.
    /// Provided by boot protocol.
    #[link_name = "rust_kernel_vaddr"]
    static mut KERNEL_VADDR: usize;
    /// Kernel base physical address.
    /// Provided by boot protocol.
    #[link_name = "rust_kernel_paddr"]
    static mut KERNEL_PADDR: usize;
}

/// Calculates the maximum page table level wherein the next part of the mapping can be made.
fn calc_superpage(virt_base: VPN, virt_len: VPN, phys_base: PPN) -> u8 {
    let align = virt_base | phys_base;
    ((align.trailing_zeros().min(virt_len.ilog2()) / BITS_PER_LEVEL as u32) as u8)
        .min(unsafe { PAGING_LEVELS as u8 })
}

/// Common implementation of [`map_k`], [`map_k_at`] and [`map_u_at`].
unsafe fn map_impl(
    pt_root_ppn: PPN,
    virt_base: VPN,
    virt_len: VPN,
    phys_base: PPN,
    flags: u32,
) -> EResult<()> {
    // Map in original page table.
    let mut offset = 0 as VPN;
    let mut root_touched = false;
    while offset < virt_len {
        let level = calc_superpage(virt_base + offset, virt_len - offset, phys_base + offset);
        root_touched |= unsafe {
            mmu::map(
                pt_root_ppn,
                virt_base + offset,
                mmu::PTE {
                    ppn: phys_base + offset,
                    flags,
                    level,
                    valid: true,
                    leaf: true,
                },
            )
        }?;
        offset += (1 as VPN) << (BITS_PER_LEVEL * level as u32);
    }

    if root_touched && (flags & flags::G) != 0 {
        let root_pte_size = 1usize << ((unsafe { PAGING_LEVELS } - 1) * BITS_PER_LEVEL);

        // Broadcast global mappings to process page tables.
        let guard = unsafe {
            SharedMutexGuard::new_raw(
                &raw mut bindings::raw::proc_mtx,
                slice_from_raw_parts(bindings::raw::procs, bindings::raw::procs_len),
            )
        };
        for proc in guard.iter() {
            let proc_pt_root = unsafe { (**proc).memmap.mem_ctx.pt_root_ppn };
            for index in
                virt_base.div(root_pte_size)..(virt_base + virt_len).div_ceil(root_pte_size)
            {
                unsafe { mmu::write_pte(proc_pt_root, index, mmu::read_pte(pt_root_ppn, index)) };
            }
        }
    }

    // Local TLB invalidation.
    let inval_range = virt_base..virt_base + virt_len;
    if inval_range.len() > cpu::mmu::INVAL_PAGE_THRESHOLD {
        cpu::mmu::vmem_fence(None, None);
    } else {
        for page in inval_range {
            cpu::mmu::vmem_fence(Some(page * PAGE_SIZE as usize), None);
        }
    }

    // TODO: Remote TLB invalidation dispatch.

    Ok(())
}

/// Map a range of memory for the kernel at a specific virtual address.
pub unsafe fn map_k_at(virt_base: VPN, virt_len: VPN, phys_base: PPN, flags: u32) -> EResult<()> {
    if flags & flags::U != 0
        || !mmu::is_canon_kernel_range(
            virt_base * PAGE_SIZE as usize..(virt_base + virt_len) * PAGE_SIZE as usize,
        )
    {
        return Err(Errno::EINVAL);
    }
    let flags = flags | flags::G;
    unsafe {
        map_impl(
            *KERNEL_PAGE_TABLE.lock(),
            virt_base,
            virt_len,
            phys_base,
            flags,
        )
    }
}

/// Finds a certain length of free pages within the higher half.
/// Makes sure there is at least one unmapped page before and after the found region.
pub unsafe fn find_free_pages(pt_root_ppn: PPN, virt_len: VPN) -> EResult<VPN> {
    let mut vpn = (mmu::higher_half_vaddr() / PAGE_SIZE as usize) as VPN;
    let limit = vpn + (mmu::canon_half_size() / PAGE_SIZE as usize) as VPN;
    let mut start_vpn = 0 as VPN;
    let mut avl_len = 0 as VPN;

    while vpn < limit {
        let pte = unsafe { mmu::walk(pt_root_ppn, vpn) };
        let pte_len = 1 << (pte.level as VPN * BITS_PER_LEVEL as VPN);
        if pte.valid {
            avl_len = 0;
        } else {
            if avl_len == 0 {
                start_vpn = vpn;
            }
            avl_len += pte_len;
        }
        if avl_len >= virt_len + 2 {
            return Ok(start_vpn + 1);
        }
        vpn += pte_len;
    }

    Err(Errno::ENOMEM)
}

/// Map a range of memory for the kernel at any virtual address.
/// Returns the virtual page number where it was mapped.
pub unsafe fn map_k(virt_len: VPN, phys_base: PPN, flags: u32) -> EResult<VPN> {
    if flags & flags::U != 0 {
        return Err(Errno::EINVAL);
    }
    let flags = flags | flags::G;
    let guard = KERNEL_PAGE_TABLE.lock();
    let virt_base = unsafe { find_free_pages(*guard, virt_len) }?;
    unsafe { map_impl(*guard, virt_base, virt_len, phys_base, flags) }?;
    Ok(virt_base)
}

/// Map a range of memory for a user page table at a specific virtual address.
pub unsafe fn map_u_at(
    pt_root_ppn: PPN,
    virt_base: VPN,
    virt_len: VPN,
    phys_base: PPN,
    flags: u32,
) -> EResult<()> {
    if flags & !(flags::RWX | flags::A | flags::D) != 0
        || !mmu::is_canon_user_range(
            virt_base * PAGE_SIZE as usize..(virt_base + virt_len) * PAGE_SIZE as usize,
        )
    {
        return Err(Errno::EINVAL);
    }
    let flags = flags | flags::U;
    unsafe { map_impl(pt_root_ppn, virt_base, virt_len, phys_base, flags) }
}

/// Describes the result of a virtual to physical address translation.
pub struct Virt2Phys {
    /// Virtual address of page start.
    pub page_vaddr: VPN,
    /// Physical address of page start.
    pub page_paddr: PPN,
    /// Size of the mapping in pages.
    pub size: usize,
    /// Physical address.
    pub paddr: usize,
    /// Flags of the mapping.
    pub flags: u32,
    /// Whether the mapping exists; if false, only `vpn` and `size` are valid.
    pub valid: bool,
}

/// Translate a virtual to a physical address.
pub unsafe fn virt2phys(pt_root_ppn: PPN, vaddr: usize) -> Virt2Phys {
    todo!()
}

/// Initialize the virtual memory subsystem.
pub unsafe fn init() {
    unsafe {
        // Detect MMU mode to be used.
        cpu::mmu::early_init();

        // Prepare new page tables containing kernel and HHDM.
        let res: EResult<()> = try {
            // Allocate and zero out page table.
            let pt_root_ppn = pmm::page_alloc(0)?;
            let tmp = PAGE_SIZE as usize;
            (&mut *slice_from_raw_parts_mut(
                (pt_root_ppn * tmp + HHDM_OFFSET) as *mut PackedPTE,
                tmp / size_of::<PackedPTE>(),
            ))
                .fill(PackedPTE::INVALID);
            *KERNEL_PAGE_TABLE.data() = pt_root_ppn;

            // Kernel RX.
            let text_vaddr = &raw const __start_text as usize;
            let text_len = &raw const __stop_text as usize - &raw const __start_text as usize;
            debug_assert!(text_len % PAGE_SIZE as usize == 0);
            map_k_at(
                text_vaddr / PAGE_SIZE as usize,
                text_len / PAGE_SIZE as usize,
                (text_vaddr - KERNEL_VADDR + KERNEL_PADDR) / PAGE_SIZE as usize,
                flags::RX | flags::G,
            )?;

            // Kernel R.
            let rodata_vaddr = &raw const __start_rodata as usize;
            let rodata_len = &raw const __stop_rodata as usize - &raw const __start_rodata as usize;
            debug_assert!(rodata_len % PAGE_SIZE as usize == 0);
            map_k_at(
                rodata_vaddr / PAGE_SIZE as usize,
                rodata_len / PAGE_SIZE as usize,
                (rodata_vaddr - KERNEL_VADDR + KERNEL_PADDR) / PAGE_SIZE as usize,
                flags::R | flags::G,
            )?;

            // Kernel RW.
            let data_vaddr = &raw const __start_data as usize;
            let data_len = &raw const __stop_data as usize - &raw const __start_data as usize;
            debug_assert!(data_len % PAGE_SIZE as usize == 0);
            map_k_at(
                data_vaddr / PAGE_SIZE as usize,
                data_len / PAGE_SIZE as usize,
                (data_vaddr - KERNEL_VADDR + KERNEL_PADDR) / PAGE_SIZE as usize,
                flags::RW | flags::G,
            )?;

            // HHDM RW.
            debug_assert!(HHDM_SIZE % PAGE_SIZE as usize == 0);
            map_k_at(
                HHDM_VADDR / PAGE_SIZE as usize,
                HHDM_SIZE / PAGE_SIZE as usize,
                (HHDM_VADDR - HHDM_OFFSET) / PAGE_SIZE as usize,
                flags::RW | flags::G,
            )?;
        };
        res.expect("Failed to create inital page table");

        // Finalize MMU initialization and switch to new page table.
        cpu::mmu::init(*KERNEL_PAGE_TABLE.data());
    }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn mem_vmm_init() {
    unsafe {
        init();
    }
}

#[unsafe(no_mangle)]
unsafe extern "C" fn mem_ctxswitch_from_isr() {
    unsafe {
        let proc = bindings::raw::proc_current();
        cpu::mmu::set_page_table((*proc).memmap.mem_ctx.pt_root_ppn, 0);
        cpu::mmu::vmem_fence(None, None);
    }
}
