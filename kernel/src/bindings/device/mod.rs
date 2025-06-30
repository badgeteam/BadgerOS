use crate::{LogLevel, bindings::raw::timestamp_us_t, logkf};
use core::{
    num::NonZero,
    ptr::{DynMetadata, NonNull, Pointee},
};

use addr::DevAddr;
use alloc::vec::Vec;
use class::{block::BlockDevice, pcictl::PciCtlDevice};

pub mod addr;
pub mod class;

use crate::bindings::raw::mutex_t;

use super::{
    error::{EResult, Errno},
    raw::{
        self, dev_addr_t, dev_class_t_DEV_CLASS_BLOCK, dev_class_t_DEV_CLASS_PCICTL,
        dev_class_t_DEV_CLASS_UNKNOWN, dev_state_t, device_block_t, device_info_t, device_pcictl_t,
        device_t, driver_t, dtb_handle_t, dtb_node_t, errno_t, file_t, irqno_t,
    },
};

/// Device info.
#[derive(Clone)]
pub struct DeviceInfo {
    pub parent: Option<Device>,
    pub irq_parent: Option<Device>,
    pub addrs: Vec<DevAddr>,
    // TODO: DTB handle.
    pub phandle: Option<NonZero<u32>>,
}

impl DeviceInfo {
    /// Converts into the format required by the C API.
    pub fn into_raw(self) -> device_info_t {
        if let Some(ref parent) = self.parent {
            unsafe { raw::device_push_ref(parent.base_ptr()) };
        }
        if let Some(ref irq_parent) = self.irq_parent {
            unsafe { raw::device_push_ref(irq_parent.base_ptr()) };
        }
        let addrs: Vec<dev_addr_t> = self.addrs.into_iter().map(Into::into).collect();
        let addrs = addrs.into_parts();
        device_info_t {
            parent: self
                .parent
                .map(|x| x.leak().as_ptr())
                .unwrap_or(0 as *mut device_t),
            irq_parent: self
                .irq_parent
                .map(|x| x.leak().as_ptr())
                .unwrap_or(0 as *mut device_t),
            addrs_len: addrs.1,
            addrs: addrs.0.as_ptr(),
            dtb_handle: 0 as *mut dtb_handle_t,
            dtb_node: 0 as *mut dtb_node_t,
            phandle: self.phandle.map(Into::into).unwrap_or(0u32),
        }
    }
}

/// View of an existing device's info.
#[derive(Clone, Copy)]
pub struct DeviceInfoView<'a> {
    inner: &'a device_info_t,
}

impl<'a> DeviceInfoView<'a> {
    pub unsafe fn from(inner: &'a device_info_t) -> Self {
        Self { inner }
    }
    pub fn parent(&self) -> Option<Device> {
        if self.inner.parent.is_null() {
            return None;
        }
        unsafe { raw::device_push_ref(self.inner.parent) }
        Some(unsafe { Device::from_raw(self.inner.parent) })
    }
    pub fn irq_parent(&self) -> Option<Device> {
        if self.inner.irq_parent.is_null() {
            return None;
        }
        unsafe { raw::device_push_ref(self.inner.irq_parent) }
        Some(unsafe { Device::from_raw(self.inner.irq_parent) })
    }
    pub fn addrs(&'a self) -> &'a [dev_addr_t] {
        unsafe { &*core::ptr::slice_from_raw_parts(self.inner.addrs, self.inner.addrs_len) }
    }
    // TODO: DTB handles.
    pub fn phandle(&self) -> Option<NonZero<u32>> {
        NonZero::try_from(self.inner.phandle).ok()
    }
}

/// Abstract base class for all devices.
pub struct AbstractDevice<T: Sized> {
    inner: NonNull<T>,
}
unsafe impl<T> Send for AbstractDevice<T> {}
unsafe impl<T> Sync for AbstractDevice<T> {}

impl<T> Drop for AbstractDevice<T> {
    fn drop(&mut self) {
        unsafe { raw::device_pop_ref(self.inner.as_ptr() as *mut device_t) }
    }
}

impl<T> Clone for AbstractDevice<T> {
    fn clone(&self) -> Self {
        unsafe { raw::device_push_ref(self.inner.as_ptr() as *mut device_t) };
        Self { inner: self.inner }
    }
}

impl<T> AbstractDevice<T> {
    /// Leak the device, returning the raw pointer.
    /// This does not decrement the refcount.
    pub fn leak(self) -> NonNull<T> {
        let ptr = self.inner;
        core::mem::forget(self);
        ptr
    }
    /// Create from a raw pointer (doesn't check type).
    /// Increments the refcount.
    pub unsafe fn from_raw_ref(inner: *mut T) -> Self {
        unsafe {
            raw::device_push_ref(inner as *mut device_t);
            Self {
                inner: NonNull::from_mut(&mut *inner),
            }
        }
    }
    /// Create from a raw pointer (doesn't check type).
    /// Doesn't increment the refcount.
    pub unsafe fn from_raw(inner: *mut T) -> Self {
        unsafe {
            Self {
                inner: NonNull::from_mut(&mut *inner),
            }
        }
    }
    /// Get the raw pointer.
    pub unsafe fn as_raw_ptr(&self) -> *mut T {
        self.inner.as_ptr()
    }
    /// Try to cast into specialized device.
    pub fn specialized(self) -> Device {
        let res = unsafe { Device::from_raw(core::mem::transmute(self.inner)) };
        core::mem::forget(self);
        res
    }
}

impl<T> HasBaseDevice for AbstractDevice<T> {
    /// Borrow as a handle to the base device type.
    fn as_base<'a>(&self) -> &'a BaseDevice {
        unsafe { core::mem::transmute(self) }
    }
    /// Get the raw pointer to the base struct.
    fn base_ptr(&self) -> *mut device_t {
        self.inner.as_ptr() as *mut device_t
    }
}

pub type BaseDevice = AbstractDevice<device_t>;

/// Helper trait to implement base device functions.
pub trait HasBaseDevice {
    /// Borrow as a handle to the base device type.
    fn as_base<'a>(&self) -> &'a BaseDevice;
    /// Get the raw pointer to the base struct.
    fn base_ptr(&self) -> *mut device_t;

    /// Get device info view.
    fn info<'a>(&'a self) -> DeviceInfoView<'a> {
        unsafe {
            DeviceInfoView {
                inner: &(*self.base_ptr()).info,
            }
        }
    }
    /// Get device ID.
    fn id(&self) -> NonZero<u32> {
        unsafe { core::mem::transmute((*self.base_ptr()).id) }
    }
    /// Get device state view.
    fn state<'a>(&'a self) -> dev_state_t {
        unsafe extern "C" {
            static mut devs_mtx: mutex_t;
        }
        unsafe {
            raw::mutex_acquire_shared(&raw mut devs_mtx, timestamp_us_t::MAX);
            let res = (*self.base_ptr()).state;
            raw::mutex_release_shared(&raw mut devs_mtx);
            res
        }
    }
    /// Get device driver view.
    fn driver<'a>(&'a self) -> *const driver_t {
        unsafe extern "C" {
            static mut devs_mtx: mutex_t;
        }
        unsafe {
            raw::mutex_acquire_shared(&raw mut (*self.base_ptr()).driver_mtx, timestamp_us_t::MAX);
            let res = (*self.base_ptr()).driver;
            raw::mutex_release_shared(&raw mut (*self.base_ptr()).driver_mtx);
            res
        }
    }
    /// Try to activate this device.
    /// Does nothing if the device is not in-tree or already activated.
    fn activate(&self) {
        unsafe { raw::device_activate(self.base_ptr()) }
    }
    /// Try to get this as a block device.
    fn as_block(&self) -> Option<BlockDevice> {
        unsafe {
            if (*self.base_ptr()).dev_class != dev_class_t_DEV_CLASS_BLOCK {
                None
            } else {
                Some(BlockDevice::from_raw_ref(
                    self.base_ptr() as *mut device_block_t
                ))
            }
        }
    }
    /// Try to get this as a PCI controller.
    fn as_pcictl(&self) -> Option<PciCtlDevice> {
        unsafe {
            if (*self.base_ptr()).dev_class != dev_class_t_DEV_CLASS_PCICTL {
                None
            } else {
                Some(PciCtlDevice::from_raw_ref(
                    self.base_ptr() as *mut device_pcictl_t
                ))
            }
        }
    }
    /// Cascade-enable an outgoing interrupt.
    unsafe fn cascase_enable_irq_out(&self, irqno: irqno_t) -> EResult<()> {
        unsafe { Errno::check(raw::device_cascade_enable_irq_out(self.base_ptr(), irqno)) }
    }
    /// Helper to send an interrupt to all children on a certain designator.
    /// Returns true if an interrupt handler was run.
    unsafe fn forward_interrupt(&self, in_irqno: irqno_t) -> bool {
        unsafe { raw::device_forward_interrupt(self.base_ptr(), in_irqno) }
    }
}

/// Enum that encapsulates all types of device.
#[derive(Clone)]
#[repr(u32, C)]
pub enum Device {
    Unknown(BaseDevice) = dev_class_t_DEV_CLASS_UNKNOWN,
    Block(BlockDevice) = dev_class_t_DEV_CLASS_BLOCK,
    PciCtl(PciCtlDevice) = dev_class_t_DEV_CLASS_PCICTL,
}

impl Device {
    /// Leak the device, returning the raw pointer.
    /// This does not decrement the refcount.
    pub fn leak(self) -> NonNull<device_t> {
        unsafe {
            match self {
                Device::Unknown(x) => x.leak(),
                Device::Block(x) => core::mem::transmute(x.leak()),
                Device::PciCtl(x) => core::mem::transmute(x.leak()),
            }
        }
    }
    /// Create a device enum from a raw pointer.
    /// Doesn't increment the refcount.
    pub unsafe fn from_raw(inner: *mut device_t) -> Self {
        unsafe {
            #[allow(non_upper_case_globals)]
            match (*inner).dev_class {
                dev_class_t_DEV_CLASS_UNKNOWN => Device::Unknown(BaseDevice::from_raw(inner)),
                dev_class_t_DEV_CLASS_BLOCK => {
                    Device::Block(BlockDevice::from_raw(inner as *mut device_block_t))
                }
                dev_class_t_DEV_CLASS_PCICTL => {
                    Device::PciCtl(PciCtlDevice::from_raw(inner as *mut device_pcictl_t))
                }
                _ => {
                    logkf!(
                        LogLevel::Warning,
                        "Unknown device class {}",
                        (*inner).dev_class
                    );
                    Device::Unknown(BaseDevice::from_raw(inner))
                }
            }
        }
    }
    /// Create a device enum from a raw pointer.
    /// Increments the refcount.
    pub unsafe fn from_raw_ref(inner: *mut device_t) -> Self {
        unsafe {
            raw::device_push_ref(inner);
            Self::from_raw(inner)
        }
    }
    /// Add a new device.
    pub fn add(info: DeviceInfo) -> EResult<BaseDevice> {
        let raw = unsafe { raw::device_add(info.into_raw()) };
        if raw.is_null() {
            Err(Errno::ENOMEM)
        } else {
            Ok(unsafe { BaseDevice::from_raw(raw) })
        }
    }
    /// Remove a device by ID.
    pub fn remove(id: NonZero<u32>) -> bool {
        unsafe { raw::device_remove(id.into()) }
    }
    /// Add a device interrupt link; child is the device that generates the interrupt, parent the one that receives it.
    /// Any device interrupt designator can be connected to any number of opposite designators, but the resulting graph must be acyclic.
    pub unsafe fn link_irq(
        child: &dyn HasBaseDevice,
        child_irqno: irqno_t,
        parent: &dyn HasBaseDevice,
        parent_irqno: irqno_t,
    ) -> EResult<()> {
        Errno::check(unsafe {
            raw::device_link_irq(
                child.base_ptr(),
                child_irqno,
                parent.base_ptr(),
                parent_irqno,
            )
        })
    }
    /// Remove a device interrupt link; see [`Device::link_irq`].
    pub unsafe fn unlink_irq(
        child: &dyn HasBaseDevice,
        child_irqno: irqno_t,
        parent: &dyn HasBaseDevice,
        parent_irqno: irqno_t,
    ) -> EResult<()> {
        Errno::check(unsafe {
            raw::device_unlink_irq(
                child.base_ptr(),
                child_irqno,
                parent.base_ptr(),
                parent_irqno,
            )
        })
    }
}

impl HasBaseDevice for Device {
    /// Borrow as a handle to the base device type.
    fn as_base<'a>(&self) -> &'a BaseDevice {
        match self {
            Device::Unknown(x) => x.as_base(),
            Device::Block(x) => x.as_base(),
            Device::PciCtl(x) => x.as_base(),
        }
    }

    /// Get the raw pointer to the base struct.
    fn base_ptr(&self) -> *mut device_t {
        match self {
            Device::Unknown(x) => x.base_ptr(),
            Device::Block(x) => x.base_ptr(),
            Device::PciCtl(x) => x.base_ptr(),
        }
    }
}

/// Common device driver functions.
pub trait BaseDriver: Sync {
    /// Remove a device from this driver; only called once.
    fn remove(&mut self) {}
    /// [optional] Called after a direct child device is added with [`Device::add`].
    /// If this fails, the child is removed again.
    fn child_added(&mut self, _child: BaseDevice) -> EResult<()> {
        Ok(())
    }
    /// [optional] Called after a direct child is activated with [`HasBaseDevice::activate`].
    fn child_activated(&mut self, _child: BaseDevice) {}
    /// [optional] Called after a direct child device gets added to a driver.
    fn child_got_driver(&mut self, _child: BaseDevice) {}
    /// [optional] Called before a direct child device gets removed from a driver.
    /// Always called before `child_removed`.
    fn child_lost_driver(&mut self, _child: BaseDevice) {}
    /// [optional] Called before a direct child device is removed with [`Device::remove`].
    fn child_removed(&mut self, _child: BaseDevice) {}
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
    fn create_devnodes(&mut self, _devtmpfs_root: file_t, _devnode_dir: file_t) -> EResult<()> {
        Ok(())
    }
}

/// Helper macro for filling in driver fields.
#[macro_export]
macro_rules! abstract_driver_struct {
    ($type: ty, $class: expr, $match_: expr, $add: expr) => {{
        use crate::bindings::{error::*, device::*, raw::*};
        use ::alloc::boxed::Box;
        use ::core::ffi::c_void;
        driver_t {
            dev_class: $class,
            match_: {
                /// Convert the types and call the matching function.
                extern "C" fn match_wrapper(info: *mut device_info_t) -> bool {
                    $match_(unsafe { DeviceInfoView::from(&*info) })
                }
                Some(match_wrapper)
            },
            add: {
                /// Convert the types and call the driver's add function and unbox the object if successful.
                extern "C" fn add_wrapper(
                    device: *mut device_t,
                ) -> errno_t {
                    let res: EResult<Box<$type>> = $add(unsafe { Device::from_raw_ref(device) });
                    match res {
                        Ok(b) => {
                            let ptr = Box::into_raw(b);
                            unsafe {
                                (*device).cookie = ptr as *mut c_void;
                            }
                            0
                        }
                        Err(errno) => -(errno as u32 as i32),
                    }
                }
                Some(add_wrapper)
            },
            remove: {
                unsafe extern "C" fn remove_wrapper(device: *mut device_t) {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    ptr.remove()
                }
                Some(remove_wrapper)
            },
            child_added: {
                unsafe extern "C" fn child_added_wrapper(device: *mut device_t, child: *mut device_t) -> errno_t {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    let child = unsafe {BaseDevice::from_raw_ref(child)};
                    Errno::extract(ptr.child_added(child))
                }
                Some(child_added_wrapper)
            },
            child_activated: {
                unsafe extern "C" fn child_activated_wrapper(device: *mut device_t, child: *mut device_t) {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    let child = unsafe {BaseDevice::from_raw_ref(child)};
                    ptr.child_activated(child)
                }
                Some(child_activated_wrapper)
            },
            child_got_driver: {
                unsafe extern "C" fn child_got_driver_wrapper(device: *mut device_t, child: *mut device_t) {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    let child = unsafe {BaseDevice::from_raw_ref(child)};
                    ptr.child_got_driver(child)
                }
                Some(child_got_driver_wrapper)
            },
            child_lost_driver: {
                unsafe extern "C" fn child_lost_driver_wrapper(device: *mut device_t, child: *mut device_t) {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    let child = unsafe {BaseDevice::from_raw_ref(child)};
                    ptr.child_lost_driver(child)
                }
                Some(child_lost_driver_wrapper)
            },
            child_removed: {
                unsafe extern "C" fn child_removed_wrapper(device: *mut device_t, child: *mut device_t) {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    let child = unsafe {BaseDevice::from_raw_ref(child)};
                    ptr.child_removed(child)
                }
                Some(child_removed_wrapper)
            },
            interrupt: {
                unsafe extern "C" fn interrupt_wrapper(device: *mut device_t, irqno: irqno_t) -> bool {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    ptr.interrupt(irqno)
                }
                Some(interrupt_wrapper)
            },
            enable_irq_out: {
                unsafe extern "C" fn enable_irq_out_wrapper(device: *mut device_t, irqno: irqno_t, enable: bool) -> errno_t {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    Errno::extract(ptr.enable_irq_out(irqno, enable))
                }
                Some(enable_irq_out_wrapper)
            },
            enable_irq_in: {
                unsafe extern "C" fn enable_irq_in_wrapper(device: *mut device_t, irqno: irqno_t, enable: bool) -> errno_t {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    Errno::extract(ptr.enable_irq_in(irqno, enable))
                }
                Some(enable_irq_in_wrapper)
            },
            cascase_enable_irq: {
                unsafe extern "C" fn cascase_enable_irq_wrapper(device: *mut device_t, irqno: irqno_t) -> errno_t {
                    let ptr = unsafe{&mut *((*device).cookie as *mut $type)};
                    Errno::extract(ptr.cascase_enable_irq(irqno))
                }
                Some(cascase_enable_irq_wrapper)
            },
            create_devnodes: None,
        }
    }};
}

/// Helper macro for filling in base driver fields.
#[macro_export]
macro_rules! base_driver_struct {
    ($type: ty, $match_: expr, $add: expr) => {
        crate::abstract_driver_struct!(
            $type,
            crate::bindings::raw::dev_class_t_DEV_CLASS_UNKNOWN,
            $match_,
            $add
        )
    };
}

pub fn add_driver(driver: &'static driver_t) -> EResult<()> {
    Errno::check(unsafe { raw::driver_add(driver) })
}

pub fn remove_driver(driver: &'static driver_t) -> EResult<()> {
    Errno::check(unsafe { raw::driver_remove(driver) })
}
