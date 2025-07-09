use alloc::{string::String, vec::Vec};

use crate::{LogLevel, bindings::error::EResult};

/// Map of kernel parameters.
static mut KPARAMS: Vec<(String, String)> = Vec::new();

/// Add a kernel parameter; should only be run on startup just after the heap is initialized.
pub unsafe fn add_kparam(key: String, raw_value: String) -> EResult<()> {
    let kparams = unsafe { &mut *&raw mut KPARAMS };
    match kparams.binary_search_by(|ent| ent.0.cmp(&key)) {
        Ok(_) => {
            logkf!(LogLevel::Warning, "Duplicate parameter {} ignored", key);
        }
        Err(index) => {
            kparams.try_reserve(kparams.len() + 1)?;
            kparams.insert(index, (key, raw_value));
        }
    };
    Ok(())
}

/// Look up a kernel parameter.
pub fn get_kparam(key: &str) -> Option<&'static str> {
    let kparams = unsafe { &*&raw const KPARAMS };
    match kparams.binary_search_by(|ent| ent.0.as_str().cmp(key)) {
        Ok(index) => Some(kparams[index].1.as_str()),
        Err(_) => None,
    }
}
