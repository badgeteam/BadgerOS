use alloc::{boxed::Box, string::String, vec::Vec};

use crate::{
    bindings::{
        device::class::block::BlockDevice,
        error::EResult,
        mutex::Mutex,
        raw::{get_volume_info_t, kmodule_t, partition_t, volume_info_t},
    },
    filesystem::partition::{gpt::GptDriver, mbr::MbrDriver},
};

pub mod gpt;
pub mod mbr;

/// Describes a single partition.
#[derive(Clone, Debug, Default)]
pub struct Partition {
    /// On-disk byte offset.
    pub offset: u64,
    /// On-disk byte size.
    pub size: u64,
    /// Type GUID.
    pub type_: u128,
    /// Partition GUI.
    pub uuid: u128,
    /// Partition name converted to UTF-8.
    pub name: String,
    /// Whether the partition is read-only.
    pub readonly: bool,
}

impl Into<partition_t> for Partition {
    fn into(self) -> partition_t {
        let name = self.name.into_raw_parts();
        partition_t {
            offset: self.offset,
            size: self.size,
            type_: [self.type_ as u64, (self.type_ >> 64) as u64],
            uuid: [self.uuid as u64, (self.uuid >> 64) as u64],
            name: name.0,
            name_len: name.1,
            readonly: self.readonly,
        }
    }
}

/// Describes the partitioning system on a particular volume.
#[derive(Clone, Debug, Default)]
pub struct VolumeInfo {
    /// Array of partitions.
    pub parts: Vec<Partition>,
    /// Volume label / name.
    pub name: String,
    /// Disk UUID.
    pub uuid: u128,
}

impl Into<volume_info_t> for VolumeInfo {
    fn into(self) -> volume_info_t {
        let parts: Vec<partition_t> = self.parts.into_iter().map(Into::into).collect();
        let parts = parts.into_raw_parts();
        let name = self.name.into_raw_parts();
        volume_info_t {
            parts: parts.0,
            parts_len: parts.1,
            name: name.0,
            name_len: name.1,
            uuid: [self.uuid as u64, (self.uuid >> 64) as u64],
        }
    }
}

impl Into<get_volume_info_t> for EResult<Option<VolumeInfo>> {
    fn into(self) -> get_volume_info_t {
        match self {
            Err(x) => get_volume_info_t {
                info: VolumeInfo::default().into(),
                errno: -(x as i32),
            },
            Ok(x) => match x {
                None => get_volume_info_t {
                    info: VolumeInfo::default().into(),
                    errno: 0,
                },
                Some(y) => get_volume_info_t {
                    info: y.into(),
                    errno: 1,
                },
            },
        }
    }
}

/// A partitioning system.
pub trait PartitionDriver {
    /// Detect this partitioning system on a medium and if present return the partitions.
    fn detect(&self, drive: BlockDevice) -> EResult<Option<VolumeInfo>>;
}

/// Set of partition system drivers.
pub static PARTITION_DRIVERS: Mutex<Vec<Box<dyn PartitionDriver>>, true> =
    unsafe { Mutex::new_static(Vec::new()) };

/// Get the volume information for a particular drive.
pub fn get_volume_info(drive: BlockDevice) -> EResult<Option<VolumeInfo>> {
    for driver in &*PARTITION_DRIVERS.lock_shared() {
        if let Some(data) = driver.detect(drive.clone())? {
            return Ok(Some(data));
        }
    }
    Ok(None)
}

register_kmodule!(partitioning, [1, 0, 0], || {
    let mut guard = PARTITION_DRIVERS.lock();
    guard.push(Box::new(GptDriver {}));
    guard.push(Box::new(MbrDriver {}));
});

mod c_api {
    use crate::{
        LogLevel,
        bindings::{
            device::{DeviceFromRaw, class::block::BlockDevice},
            raw::{device_block_t, get_volume_info_t},
        },
    };

    // Get the volume information for a particular drive.
    #[unsafe(no_mangle)]
    unsafe extern "C" fn get_volume_info(device: *mut device_block_t) -> get_volume_info_t {
        let dev = unsafe { BlockDevice::from_raw_ref(device) };
        let info = super::get_volume_info(dev);
        logkf!(LogLevel::Debug, "Volume info: {:?}", &info);
        info.into()
    }
}
