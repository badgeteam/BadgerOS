use core::mem::offset_of;

use super::{ctrl::AhciDriver, hms, reg};
use alloc::boxed::Box;
use tock_registers::{
    interfaces::{ReadWriteable, Readable, Writeable},
    register_structs,
    registers::ReadWrite,
};

use crate::{
    LogLevel,
    bindings::{
        self,
        device::{
            BaseDriver, Device, DeviceInfoView, HasBaseDevice,
            addr::DevAddr,
            class::block::{BlockDevice, BlockDriver},
        },
        error::{EResult, Errno},
        mutex::Mutex,
        pmm::PhysBox,
        raw::{driver_block_t, mpu_global_ctx},
        time_us,
    },
    block_driver, config,
    device::builtin_driver::ahci::{AHCI_DRIVER, ata, fis},
    logk,
};

register_structs! {
    /// How BadgerOS chooses to lay out the host memory structs.
    pub PortHMS {
        /// Command list.
        (0x000 => pub cmd_list: [hms::CmdHdr; 32]),
        /// Received FIS.
        (0x400 => pub rxfis:    [ReadWrite<u32>; 64]),
        /// Command FIS.
        (0x500 => pub cmd_fis:  [ReadWrite<u32>; 16]),
        /// Reserved.
        (0x540 => _resvd0:      [u8; 64]),
        /// Physical region descriptor table.
        (0x580 => pub prdt:     [hms::PRDT; 65536]),
        (0x100580 => @END),
    }
}

/// Match a SATA AHCI drive.
fn sata_match(info: DeviceInfoView<'_>) -> bool {
    if info.addrs().len() == 0 {
        return false;
    }
    match info.parent() {
        Some(parent) => {
            parent.driver() == &raw const AHCI_DRIVER
                && matches!(DevAddr::from(info.addrs()[0]), DevAddr::Ahci(_))
        }
        None => false,
    }
}

/// Host memory structs and MMIO reference.
pub(super) struct SataMem {
    pub(super) hms: PhysBox<PortHMS>,
    pub(super) mmio: &'static reg::Port,
}

/// The SATA AHCI drive driver.
pub(super) struct SataDriver {
    pub(super) device: BlockDevice,
    pub(super) parent: &'static mut AhciDriver,
    pub(super) mem: Mutex<SataMem, false>,
    pub(super) supports_48bit: bool,
}

impl SataDriver {
    pub fn new(device: Device) -> EResult<Box<Self>> {
        // Assert all preconditions and get handles.
        let device = device.as_block().unwrap();
        let parent_dev = device.info().parent().unwrap();
        assert!(parent_dev.driver() == &raw const AHCI_DRIVER);
        let parent: &'static mut AhciDriver =
            unsafe { &mut *core::ptr::from_raw_parts_mut((*parent_dev.base_ptr()).cookie, ()) };
        let mmio = &parent.ports_mmio
            [unsafe { device.info().addrs()[0].__bindgen_anon_1.ahci.port } as usize];

        // Allocate memory for this port.
        let hms = unsafe { PhysBox::<PortHMS>::try_new(false, true) }?;

        // Allocate the BOX.
        let mut this = Box::try_new(Self {
            device,
            parent,
            mem: Mutex::new(SataMem { hms, mmio }),
            supports_48bit: false,
        })?;

        {
            let guard = this.mem.lock();

            // Set up the physical-memory pointers.
            let cmd_paddr = offset_of!(PortHMS, cmd_fis) + guard.hms.paddr();
            guard.hms.cmd_list[0]
                .cmd_addr_lo
                .set((cmd_paddr & 0xffffffff) as u32);
            guard.hms.cmd_list[0]
                .cmd_addr_hi
                .set((cmd_paddr >> 32) as u32);

            let cmd_list_paddr = offset_of!(PortHMS, cmd_list) + guard.hms.paddr();
            guard
                .mmio
                .cmdlist_addr_lo
                .set((cmd_list_paddr & 0xffffffff) as u32);
            guard
                .mmio
                .cmdlist_addr_hi
                .set((cmd_list_paddr >> 32) as u32);

            let rxfis_paddr = offset_of!(PortHMS, rxfis) + guard.hms.paddr();
            guard
                .mmio
                .fis_addr_lo
                .set((rxfis_paddr & 0xffffffff) as u32);
            guard.mmio.fis_addr_hi.set((rxfis_paddr >> 32) as u32);

            guard.mmio.cmd_issue.set(0);
            guard.mmio.cmd.modify(reg::PortCmd::fis_en.val(1));
            guard.mmio.cmd.modify(reg::PortCmd::cmd_start.val(1));
        }

        logk(LogLevel::Debug, "Querying SATA device info");

        // Get device identity.
        let mut id = [0u16; 256];
        this.identify(&mut id)?;
        let dev_blk = unsafe { &mut *this.device.as_raw_ptr() };
        this.supports_48bit = id[83] & (1 << 10) != 0;
        let sec_size_exp = id[106] & 15;
        dev_blk.block_size = 1u64 << sec_size_exp;
        dev_blk.block_count = (id[100] as u64)
            + ((id[101] as u64) << 16)
            + ((id[102] as u64) << 32)
            + ((id[103] as u64) << 48);

        logkf!(
            LogLevel::Debug,
            "48-bit: {}; sec. size: {}; sec. count: {}",
            if this.supports_48bit { 'y' } else { 'n' },
            dev_blk.block_size,
            dev_blk.block_count
        );

        Ok(this)
    }
}

impl SataDriver {
    /// Issue the current command and await its completion.
    unsafe fn await_cmd(reg: &reg::Port) -> EResult<()> {
        reg.cmd_issue.set(1);
        let lim = time_us() + 10000;
        loop {
            if reg.cmd_issue.get() == 0 {
                return Ok(());
            } else if reg.irq_status.read(reg::PortIrq::tf_err) != 0 {
                reg.irq_status.write(reg::PortIrq::tf_err.val(1));
                return Err(Errno::EIO);
            } else if time_us() > lim {
                return Err(Errno::EIO);
            }
        }
    }
    /// Issue and await a command that reads data.
    unsafe fn do_raw_cmd(
        &self,
        make_cmd: impl FnOnce(*mut ()) -> u32,
        data: &mut [u8],
        is_write: bool,
    ) -> EResult<()> {
        let mut guard = self.mem.lock();

        // Ensure data is aligned to at least 2 bytes.
        if &data[0] as *const u8 as usize & 1 != 0 {
            logk(
                LogLevel::Error,
                "Misaligned address for SataDriver::issue_raw_cmd",
            );
            return Err(Errno::EINVAL);
        } else if data.len() & 1 != 0 {
            logk(LogLevel::Error, "Odd size for SataDriver::issue_raw_cmd");
            return Err(Errno::EINVAL);
        }

        // Copy in the command.
        let fis_len = make_cmd(&raw mut guard.hms.cmd_fis as *mut ());

        // Do virt2phys lookup and write PRDT entries.
        let mut offset = 0usize;
        let mut prdt = 0usize;
        while offset < data.len() {
            let v2p = unsafe {
                bindings::raw::memprotect_virt2phys(
                    &raw mut mpu_global_ctx,
                    &data[offset] as *const u8 as usize,
                )
            };
            let len = v2p.page_size + v2p.page_paddr - v2p.paddr;
            let len = len.min(data.len() - offset);

            guard.hms.prdt[prdt].paddr.set(v2p.paddr as u64);
            guard.hms.prdt[prdt]
                .dbc
                .write(hms::DBC::count.val(len as u32 - 1));

            prdt += 1;
            offset += len;
        }

        // Write command descriptor.
        guard.hms.cmd_list[0].desc.write(
            hms::CmdHdrDesc::clr_busy.val(1)
                + hms::CmdHdrDesc::write.val(is_write as u32)
                + hms::CmdHdrDesc::fis_len.val(fis_len as u32)
                + hms::CmdHdrDesc::pmp.val(unsafe {
                    self.device.info().addrs()[0]
                        .__bindgen_anon_1
                        .ahci
                        .pmul_port as u32
                }),
        );
        guard.hms.cmd_list[0].prd_len.set(data.len() as u32);

        // Issue and await the command.
        guard.mmio.cmd_issue.set(1);
        let lim = time_us() + 100000;
        loop {
            if guard.mmio.cmd_issue.get() & 1 == 0 {
                return Ok(());
            } else if guard.mmio.irq_status.read(reg::PortIrq::tf_err) != 0 {
                guard.mmio.irq_status.write(reg::PortIrq::tf_err.val(1));
                logk(LogLevel::Error, "SATA AHCI task-file error reported");
                return Err(Errno::EIO);
            } else if time_us() > lim {
                logk(LogLevel::Error, "SATA AHCI command timed out");
                return Err(Errno::EIO);
            }
        }
    }
    /// Issue an ATA command.
    unsafe fn do_ata_cmd(
        &self,
        cmd: ata::Command,
        ctrl: u8,
        sec_count: u16,
        feature: u16,
        lba: u64,
        data: &mut [u8],
        is_write: bool,
    ) -> EResult<()> {
        let lba = lba.to_le_bytes();
        unsafe {
            self.do_raw_cmd(
                |ptr| {
                    let fis = &mut *(ptr as *mut fis::RegisterH2D);
                    fis.lba[0].set(lba[0]);
                    fis.lba[1].set(lba[1]);
                    fis.lba[2].set(lba[2]);
                    fis.lba_exp[0].set(lba[3]);
                    fis.lba_exp[1].set(lba[4]);
                    fis.lba_exp[2].set(lba[5]);
                    fis.fis_type.set(fis::Type::RegisterH2D as u8);
                    fis.command.set(cmd as u8);
                    fis.device.set(ctrl);
                    fis.features.set(feature as u8);
                    fis.features_exp.set((feature >> 8) as u8);
                    fis.pmc.write(
                        fis::PMC::cmdr_xfer.val(1)
                            + fis::PMC::pm_port.val(
                                self.device.info().addrs()[0]
                                    .__bindgen_anon_1
                                    .ahci
                                    .pmul_port,
                            ),
                    );
                    fis.sec_count.set(sec_count);
                    return (size_of::<fis::RegisterH2D>() / 4) as u32;
                },
                data,
                is_write,
            )
        }
    }
    /// Get device information.
    pub fn identify(&self, id_out: &mut [u16; 256]) -> EResult<()> {
        unsafe {
            self.do_ata_cmd(
                ata::Command::IdentDev,
                0,
                0,
                0,
                0,
                bytemuck::cast_slice_mut(id_out),
                false,
            )
        }
    }
}

impl BaseDriver for SataDriver {}

impl BlockDriver for SataDriver {
    fn write_blocks(&self, start: u64, count: u64, data: &[u8]) -> EResult<()> {
        todo!()
    }

    fn read_blocks(&self, start: u64, count: u64, data: &mut [u8]) -> EResult<()> {
        todo!()
    }

    fn is_block_erased(&self, start: u64) -> EResult<()> {
        todo!()
    }

    fn erase_blocks(
        &self,
        start: u64,
        count: u64,
        mode: crate::bindings::raw::blkdev_erase_t,
    ) -> EResult<()> {
        todo!()
    }
}

/// The SATA drive driver struct.
pub(super) static SATA_DRIVER: driver_block_t = block_driver!(sata_match, SataDriver::new);
