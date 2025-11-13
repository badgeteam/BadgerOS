// SPDX-FileCopyrightText: 2025 Julian Scheffers <julian@scheffers.net>
// SPDX-FileType: SOURCE
// SPDX-License-Identifier: MIT

use addr::DeviceAddr;
use alloc::{collections::btree_map::BTreeMap, rc::Rc, sync::Arc, vec::Vec};
use class::Specialty;
use dtb::fdt;

use crate::{
    bindings::{
        error::{EResult, Errno},
        mutex::Mutex,
        raw::irqno_t,
    },
    filesystem::File,
};

pub mod addr;
pub mod builtin_driver;
pub mod class;
pub mod dtb;
pub mod irq;

/// Abstract information about devices.
pub struct DeviceInfo {
    /// Parent device, if any.
    pub parent: Option<Arc<Device>>,
    /// Interrupt parent device, if any.
    /// Should not be set for devices that forward interrupts to multiple different interrupt controllers.
    pub irq_parent: Option<Arc<Device>>,
    /// Device addresses.
    pub addrs: Vec<DeviceAddr>,
    /// DTB node handle, if any.
    pub dtb_node: Option<Rc<fdt::Node>>,
}

/// Device lifecycle state.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeviceLifecycle {
    /// Being set up for later driver assignment and usage.
    Inactive,
    /// Currently eligible for receiving a driver and then being used.
    Active,
    /// Device is no longer physically in the system.
    Removed,
}

/// Dynamic device state that may change over time.
pub struct DeviceState {
    /// What stage in the lifecycle the device is in.
    pub stage: DeviceLifecycle,
    /// What the device specializes in and additional information for said specialty.
    pub specialty: Specialty,
}

/// Management interface for devices.
pub struct Device {
    /// Device's abstract info.
    pub info: DeviceInfo,
    /// Globally unique device ID.
    pub id: u32,
    /// Dynamic device state.
    pub state: Mutex<DeviceState>,
    /// Map of incoming interrupt signals to interrupt children.
    pub irq_children: BTreeMap<irqno_t, (Arc<Device>, irqno_t)>,
    /// Map of outgoing interrupt signals to interrupt parents.
    pub irq_parents: BTreeMap<irqno_t, (Arc<Device>, irqno_t)>,
}

/// Common device driver functions.
pub trait BaseDriver: Sync {
    /// Remove a device from this driver; only called once.
    fn remove(&mut self, _device: &Device) {}
    /// [optional] Called after a direct child device is added with [`Device::add`].
    /// If this fails, the child is removed again.
    fn child_added(&mut self, _child: &Device) -> EResult<()> {
        Ok(())
    }
    /// [optional] Called after a direct child is activated.
    fn child_activated(&mut self, _child: &Device) {}
    /// [optional] Called after a direct child device gets added to a driver.
    fn child_got_driver(&mut self, _child: &Device) {}
    /// [optional] Called before a direct child device gets removed from a driver.
    /// Always called before `child_removed`.
    fn child_lost_driver(&mut self, _child: &Device) {}
    /// [optional] Called before a direct child device is removed with [`Device::remove`].
    fn child_removed(&mut self, _child: &Device) {}
    /// Device interrupt handler; also responsible for any potential forwarding of interrupts.
    /// Only called from an interrupt context.
    /// Returns true if this handled an interrupt request.
    fn interrupt(&mut self, _irq: irqno_t) -> bool {
        false
    }
    /// Enable a certain interrupt output.
    /// Can be called with interrupts disabled.
    fn enable_irq_out(&mut self, _irq: irqno_t, _enable: bool) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
    /// [optional] Enable an incoming interrupt.
    /// Can be called with interrupts disabled.
    fn enable_irq_in(&mut self, _irq: irqno_t, _enable: bool) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
    /// [optional] Cascade-enable interrupts from some input designator.
    /// Can be called with interrupts disabled.
    fn cascase_enable_irq(&mut self, _irq: irqno_t) -> EResult<()> {
        Err(Errno::ENOTSUP)
    }
    /// [optional] Create additional device node files.
    /// Called when a new `devtmpfs` is mounted OR after registered to the driver.
    fn create_devnodes(
        &mut self,
        _devtmpfs_root: &dyn File,
        _devnode_dir: &dyn File,
    ) -> EResult<()> {
        Ok(())
    }
}
