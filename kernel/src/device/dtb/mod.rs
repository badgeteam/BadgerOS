// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use core::ffi::c_void;

use fdt::Fdt;

pub mod fdt;

/// Initialize the device subsystem on a DTB platform.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn device_init_dtb(fdt_ptr: *const c_void) {
    let dtb = unsafe { Fdt::parse(fdt_ptr as *const ()) }.unwrap();
    dtb.root.debug_print(0);
}
