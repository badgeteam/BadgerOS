use limine::request::ExecutableFileRequest;

use crate::bindings::{device::class::block::BlockDevice, error::EResult};

#[unsafe(link_section = ".requests")]
pub static KERNEL_FILE: ExecutableFileRequest = ExecutableFileRequest::new();

/// Try to find the drive that the kernel was loaded from.
fn find_kernel_drive() -> Option<BlockDevice> {
    let kernel_file = KERNEL_FILE.get_response();
    None
}

/// Mount the root filesystem according to kernel parameters.
fn mount_root_fs() -> EResult<()> {
    todo!();
}

mod c_api {
    use crate::{
        bindings::{error::Errno, raw::errno_t},
        filesystem::mount_root::mount_root_fs,
    };

    #[unsafe(no_mangle)]
    /// Mount the root filesystem according to kernel parameters.
    unsafe extern "C" fn fs_mount_root_fs() -> errno_t {
        Errno::extract(mount_root_fs())
    }
}
