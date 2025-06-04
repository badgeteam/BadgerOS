use core::mem::MaybeUninit;

use alloc::{boxed::Box, vec};
use tock_registers::interfaces::{ReadWriteable, Readable};
mod reg;

use crate::{
    LogLevel, base_driver,
    bindings::{
        device::{
            BaseDriver, Device, DeviceInfo, DeviceInfoView, HasBaseDevice, add_driver,
            class::pcictl::{MappedBar, PciCtlDevice},
        },
        error::{EResult, Errno},
        raw::{
            dev_addr_t, dev_addr_t__bindgen_ty_1, dev_ahci_addr_t, dev_atype_t_DEV_ATYPE_AHCI,
            dev_atype_t_DEV_ATYPE_PCI, dev_class_t_DEV_CLASS_UNKNOWN, driver_t,
            pci_bclass_t_PCI_BCLASS_STORAGE,
            pci_progif_storage_sata_t_PCI_PROGIF_STORAGE_SATA_AHCI,
            pci_subclass_storage_t_PCI_SUBCLASS_STORAGE_SATA, pcie_hdr_com_t,
        },
        thread::Thread,
        time_us,
    },
    logk,
};

/// Match an AHCI controller.
fn ahci_match(info: DeviceInfoView<'_>) -> bool {
    // Ensure that parent is a PCI controller.
    if info.parent().is_none() {
        return false;
    }
    let parent = info.parent().unwrap().as_pcictl();
    if parent.is_none() {
        return false;
    }
    let parent = parent.unwrap();

    // Check that function 0 is AHCI.
    let addr = info.addrs()[0];
    if addr.type_ != dev_atype_t_DEV_ATYPE_PCI {
        return false;
    }
    let addr = unsafe { addr.__bindgen_anon_1.pci };

    // Read the PCIe header so we can tell the type of device this is.
    let mut header = MaybeUninit::<pcie_hdr_com_t>::uninit();
    unsafe { parent.dev_cam_read(addr, 0, &mut header) };
    let header = unsafe { header.assume_init() };

    // Check that this is a SATA AHCI controller.
    if header.classcode.baseclass != pci_bclass_t_PCI_BCLASS_STORAGE
        || header.classcode.subclass != pci_subclass_storage_t_PCI_SUBCLASS_STORAGE_SATA
        || header.classcode.progif != pci_progif_storage_sata_t_PCI_PROGIF_STORAGE_SATA_AHCI
    {
        return false;
    }

    true
}

/// The AHCI controller driver.
struct AhciDriver {
    parent: PciCtlDevice,
    device: Device,
    bar_mapping: MappedBar,
    mmio: &'static reg::Host,
    ports_mmio: &'static [reg::Port; 32],
}

impl AhciDriver {
    pub fn new(device: Device) -> EResult<Box<Self>> {
        // Get the parent device, which should be a PCI controller.
        let parent = device
            .info()
            .parent()
            .and_then(|x| x.as_pcictl())
            .ok_or(Errno::EINVAL)?;

        let addr = unsafe { device.info().addrs()[0].__bindgen_anon_1.pci };
        logkf!(
            LogLevel::Info,
            "Detected SATA AHCI controller at {:02x}:{:02x}.{:o}",
            unsafe { addr.__bindgen_anon_1.bus() },
            unsafe { addr.__bindgen_anon_1.dev() },
            unsafe { addr.__bindgen_anon_1.func() }
        );

        // Map BAR no. 5.
        let bar_info = unsafe { parent.bar_info(addr) };
        let bar_mapping = unsafe { parent.bar_map(bar_info[5]) }?;
        let mmio = unsafe { &*(bar_mapping.as_ptr() as *const reg::Host) };
        let ports_mmio =
            unsafe { &*(bar_mapping.as_ptr().wrapping_add(0x100) as *const [reg::Port; 32]) };

        // Perform BIOS/OS handoff (if required).
        if mmio.cap2.read(reg::HostCapsExt::supports_bios_handoff) != 0 {
            let lim = time_us() + 2000000;
            mmio.bohc.modify(reg::HostBOHC::os_owned.val(1));
            while mmio.bohc.read(reg::HostBOHC::bios_owned) != 0
                || mmio.bohc.read(reg::HostBOHC::bios_busy) != 0
            {
                if time_us() > lim {
                    logk(LogLevel::Warning, "Failed to take HBA ownership");
                    return Err(Errno::ENAVAIL);
                }
                Thread::sleep_us(10000);
            }
        }

        // Reset the HBA.
        mmio.ghc.modify(reg::HostCtrl::hba_reset.val(1));
        let lim = time_us() + 1000000;
        while mmio.ghc.read(reg::HostCtrl::hba_reset) != 0 {
            if time_us() > lim {
                logk(LogLevel::Warning, "Failed to reset HBA");
                return Err(Errno::ENAVAIL);
            }
            Thread::sleep_us(10000);
        }

        // Switch to AHCI mode if it wasn't already in that mode.
        mmio.ghc.modify(reg::HostCtrl::ahci_en.val(1));

        // Detect drives in ports.
        for i in 0..32 {
            if ports_mmio[i].sstatus.read(reg::PortSStatus::detect) != 0 {
                logkf!(LogLevel::Info, "Found drive in slot {}", i);
                let res = Device::add(DeviceInfo {
                    parent: Some(device.clone()),
                    irq_parent: Some(device.clone()),
                    addrs: vec![dev_addr_t {
                        type_: dev_atype_t_DEV_ATYPE_AHCI,
                        __bindgen_anon_1: dev_addr_t__bindgen_ty_1 {
                            ahci: dev_ahci_addr_t {
                                port: i as u8,
                                pmul_port: 0,
                                pmul: false,
                            },
                        },
                    }],
                    phandle: None,
                });
                match res {
                    Err(x) => logkf!(LogLevel::Error, "Could not add drive {}: {}", i, x),
                    Ok(x) => x.activate(),
                }
            }
        }

        Ok(Box::new(Self {
            parent,
            device,
            bar_mapping,
            mmio,
            ports_mmio,
        }))
    }
}

impl BaseDriver for AhciDriver {
    fn remove(&mut self) {
        todo!()
    }

    fn interrupt(&mut self, _irq: crate::bindings::raw::irqno_t) -> bool {
        todo!()
    }

    fn enable_irq_out(&mut self, irq: crate::bindings::raw::irqno_t, enable: bool) -> EResult<()> {
        if irq >= 32 {
            return Err(Errno::EINVAL);
        }
        todo!()
    }

    fn enable_irq_in(&mut self, _irq: crate::bindings::raw::irqno_t, _enable: bool) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
}

/// The AHCI driver struct.
static AHCI_DRIVER: driver_t =
    base_driver!(dev_class_t_DEV_CLASS_UNKNOWN, ahci_match, AhciDriver::new);

/// Add the AHCI driver.
pub fn add_drivers() {
    let _ = add_driver(&AHCI_DRIVER);
}
