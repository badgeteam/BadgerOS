use crate::{LogLevel, logkf};
use core::{
    ffi::c_void,
    num::NonZero,
    ptr::{DynMetadata, NonNull, Pointee, metadata},
};

use alloc::{boxed::Box, vec::Vec};
use class::{block::BlockDevice, pcictl::PciCtlDevice};

pub mod class;

use crate::{ReadOnly, bindings::raw::mutex_t};

use super::{
    error::{EResult, Errno},
    mutex::DetachedMutex,
    raw::{
        self, dev_addr_t, dev_class_t, dev_class_t_DEV_CLASS_BLOCK, dev_class_t_DEV_CLASS_PCICTL,
        dev_class_t_DEV_CLASS_UNKNOWN, dev_state_t, device_block_t, device_info_t, device_pcictl_t,
        device_t, driver_t, dtb_handle_t, dtb_node_t, errno_t, file_t, irqno_t,
    },
};

/// Device info.
#[derive(Clone)]
pub struct DeviceInfo {
    pub parent: Option<Device>,
    pub irq_parent: Option<Device>,
    pub addrs: Vec<dev_addr_t>,
    // TODO: DTB handle.
    pub phandle: Option<NonZero<u32>>,
}

impl DeviceInfo {
    /// Converts into the format required by the C API.
    pub fn into_raw(mut self) -> device_info_t {
        if let Some(ref parent) = self.parent {
            unsafe { raw::device_push_ref(parent.base_ptr()) };
        }
        if let Some(ref irq_parent) = self.irq_parent {
            unsafe { raw::device_push_ref(irq_parent.base_ptr()) };
        }
        device_info_t {
            parent: self
                .parent
                .map(|x| unsafe { x.base_ptr() })
                .unwrap_or(0 as *mut device_t),
            irq_parent: self
                .irq_parent
                .map(|x| unsafe { x.base_ptr() })
                .unwrap_or(0 as *mut device_t),
            addrs_len: self.addrs.len(),
            addrs: self.addrs.as_mut_ptr(),
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
    unsafe fn base_ptr(&self) -> *mut device_t {
        self.inner.as_ptr() as *mut device_t
    }
}

pub type BaseDevice = AbstractDevice<device_t>;

/// Helper trait to implement base device functions.
pub trait HasBaseDevice {
    /// Borrow as a handle to the base device type.
    fn as_base<'a>(&self) -> &'a BaseDevice;
    /// Get the raw pointer to the base struct.
    unsafe fn base_ptr(&self) -> *mut device_t;

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
    fn state<'a>(&'a self) -> DetachedMutex<'a, ReadOnly<dev_state_t>, true> {
        unsafe extern "C" {
            static mut devs_mtx: mutex_t;
        }
        unsafe {
            DetachedMutex::new(
                &raw mut devs_mtx,
                ReadOnly::new(&mut (*self.base_ptr()).state),
            )
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
    /// Create a device enum from a raw pointer.
    pub unsafe fn from_raw(inner: *mut device_t) -> Self {
        unsafe {
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
    unsafe fn base_ptr(&self) -> *mut device_t {
        unsafe {
            match self {
                Device::Unknown(x) => x.base_ptr(),
                Device::Block(x) => x.base_ptr(),
                Device::PciCtl(x) => x.base_ptr(),
            }
        }
    }
}

/// Common device driver functions.
pub trait BaseDriver {
    // Remove a device from this driver; only called once.
    fn remove(&mut self);
    // [optional] Called after a direct child device is added with `device_add`.
    // If this fails, the child is removed again.
    fn child_added(&mut self, _child: BaseDevice) -> EResult<()> {
        Ok(())
    }
    // [optional] Called after a direct child is activated with `device_activate`.
    fn child_activated(&mut self, _child: BaseDevice) {}
    // [optional] Called after a direct child device gets added to a driver.
    fn child_got_driver(&mut self, _child: BaseDevice) {}
    // [optional] Called before a direct child device gets removed from a driver.
    // Always called before `child_removed`.
    fn child_lost_driver(&mut self, _child: BaseDevice) {}
    // [optional] Called before a direct child device is removed with `device_remove`.
    fn child_removed(&mut self, _child: BaseDevice) {}
    /// Device interrupt handler; also responsible for any potential forwarding of interrupts.
    /// Only called from an interrupt context.
    /// Returns true if this handled an interrupt request.
    fn interrupt(&mut self, _irq: irqno_t) -> bool;
    /// Enable a certain interrupt output.
    /// Can be called with interrupts disabled.
    fn enable_irq_out(&mut self, _irq: irqno_t, _enable: bool) -> EResult<()>;
    /// [optional] Enable an incoming interrupt.
    /// Can be called with interrupts disabled.
    fn enable_irq_in(&mut self, _irq: irqno_t, _enable: bool) -> EResult<()>;
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

pub const unsafe fn recombine_driver<
    T: ?Sized + BaseDriver + Pointee<Metadata = DynMetadata<T>>,
>(
    device: *mut device_t,
) -> &'static mut T {
    unsafe {
        &mut *(core::ptr::from_raw_parts::<T>(
            (*device).cookie,
            core::mem::transmute((*device).cookie2),
        ) as *mut T)
    }
}

// Remove a device from this driver.
pub unsafe extern "C" fn driver_t_remove_wrapper(device: *mut device_t) {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    driver.remove();
}

// [optional] Called after a direct child device is added with `device_add`.
// If this fails, the child is removed again.
pub unsafe extern "C" fn driver_t_child_added_wrapper(
    device: *mut device_t,
    child_device: *mut device_t,
) -> errno_t {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    let child_device = unsafe { BaseDevice::from_raw_ref(child_device) };
    Errno::extract(driver.child_added(child_device))
}

// [optional] Called after a direct child is activated with `device_activate`.
pub unsafe extern "C" fn driver_t_child_activated_wrapper(
    device: *mut device_t,
    child_device: *mut device_t,
) {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    let child_device = unsafe { BaseDevice::from_raw_ref(child_device) };
    driver.child_activated(child_device);
}

// [optional] Called after a direct child device gets added to a driver.
pub unsafe extern "C" fn driver_t_child_got_driver_wrapper(
    device: *mut device_t,
    child_device: *mut device_t,
) {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    let child_device = unsafe { BaseDevice::from_raw_ref(child_device) };
    driver.child_got_driver(child_device);
}

// [optional] Called before a direct child device gets removed from a driver.
// Always called before `child_removed`.
pub unsafe extern "C" fn driver_t_child_lost_driver_wrapper(
    device: *mut device_t,
    child_device: *mut device_t,
) {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    let child_device = unsafe { BaseDevice::from_raw_ref(child_device) };
    driver.child_lost_driver(child_device);
}

// [optional] Called before a direct child device is removed with `device_remove`.
pub unsafe extern "C" fn driver_t_child_removed_wrapper(
    device: *mut device_t,
    child_device: *mut device_t,
) {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    let child_device = unsafe { BaseDevice::from_raw_ref(child_device) };
    driver.child_removed(child_device);
}

// Device interrupt handler; also responsible for any potential forwarding of interrupts.
// Only called from an interrupt context.
// Returns true if this handled an interrupt request.
pub unsafe extern "C" fn driver_t_interrupt_wrapper(device: *mut device_t, irqno: irqno_t) -> bool {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    driver.interrupt(irqno)
}

// Enable a certain interrupt output.
// Can be called with interrupts disabled.
pub unsafe extern "C" fn driver_t_enable_irq_out_wrapper(
    device: *mut device_t,
    out_irqno: irqno_t,
    enable: bool,
) -> errno_t {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    Errno::extract(driver.enable_irq_out(out_irqno, enable))
}

// [optional] Enable an incoming interrupt.
// Can be called with interrupts disabled.
pub unsafe extern "C" fn driver_t_enable_irq_in_wrapper(
    device: *mut device_t,
    in_irqno: irqno_t,
    enable: bool,
) -> errno_t {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    Errno::extract(driver.enable_irq_in(in_irqno, enable))
}

// [optional] Cascade-enable interrupts from some input designator.
// Can be called with interrupts disabled.
pub unsafe extern "C" fn driver_t_cascase_enable_irq_wrapper(
    device: *mut device_t,
    in_irqno: irqno_t,
) -> errno_t {
    let driver = unsafe { recombine_driver::<dyn BaseDriver>(device) };
    Errno::extract(driver.cascase_enable_irq(in_irqno))
}

/// Helper macro for filling in base driver fields.
#[macro_export]
macro_rules! base_driver {
    ($class: expr, $match_: expr, $add: expr) => {
    crate::bindings::raw::driver_t {
        dev_class: $class,
        match_: {
            /// Convert the types and call the matching function.
            extern "C" fn wrapper(info: *mut crate::bindings::raw::device_info_t) -> bool {
                $match_(unsafe { crate::bindings::device::DeviceInfoView::from(&*info) })
            }
            Some(wrapper)
        },
        add: {
            /// Convert the types and call the driver's add function and unbox the trait object if successful.
            extern "C" fn wrapper(
                device: *mut crate::bindings::raw::device_t,
            ) -> crate::bindings::raw::errno_t {
                let res = $add(unsafe { crate::bindings::device::Device::from_raw(device) });
                match res {
                    Ok(b) => {
                        let ptr = Box::into_raw(b);
                        unsafe {
                            (*device).cookie = ptr as *mut core::ffi::c_void;
                            (*device).cookie2 =
                                core::mem::transmute(core::ptr::metadata::<
                                    dyn crate::bindings::device::BaseDriver,
                                >(&*ptr));
                        }
                        0
                    }
                    Err(errno) => -(errno as u32 as i32),
                }
            }
            Some(wrapper)
        },
        remove: Some(crate::bindings::device::driver_t_remove_wrapper),
        child_added: Some(crate::bindings::device::driver_t_child_added_wrapper),
        child_activated: Some(crate::bindings::device::driver_t_child_activated_wrapper),
        child_got_driver: Some(crate::bindings::device::driver_t_child_got_driver_wrapper),
        child_lost_driver: Some(crate::bindings::device::driver_t_child_lost_driver_wrapper),
        child_removed: Some(crate::bindings::device::driver_t_child_removed_wrapper),
        interrupt: Some(crate::bindings::device::driver_t_interrupt_wrapper),
        enable_irq_out: Some(crate::bindings::device::driver_t_enable_irq_out_wrapper),
        enable_irq_in: Some(crate::bindings::device::driver_t_enable_irq_in_wrapper),
        cascase_enable_irq: Some(crate::bindings::device::driver_t_cascase_enable_irq_wrapper),
        create_devnodes: None,
    }
    };
}

pub fn add_driver(driver: &'static driver_t) -> EResult<()> {
    Errno::check(unsafe { raw::driver_add(driver) })
}

pub fn remove_driver(driver: &'static driver_t) -> EResult<()> {
    Errno::check(unsafe { raw::driver_remove(driver) })
}
