use core::fmt::{Display, Formatter, Write};

use super::raw;

/// Log severity level.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[allow(unused)]
pub enum LogLevel {
    Fatal,
    Error,
    Warning,
    Info,
    Debug,
}

/// Print an unformatted message without locking the mutex.
pub fn logk_unlocked(level: LogLevel, msg: &str) {
    unsafe {
        let bytes = msg.as_bytes();
        raw::logk_len_from_isr(level as u32, bytes.as_ptr(), bytes.len());
    }
}

/// Print an unformatted message.
pub fn logk(level: LogLevel, msg: &str) {
    unsafe {
        let bytes = msg.as_bytes();
        raw::logk_len(level as u32, bytes.as_ptr(), bytes.len());
    }
}

/// Dummy struct used to send format results to the log output.
struct LogWriter {}

impl Write for LogWriter {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let bytes = s.as_bytes();
        unsafe { raw::rawprint_substr(bytes.as_ptr(), bytes.len()) };
        Ok(())
    }
}

/// Print a formatted message without locking the mutex.
pub fn logkf_unlocked(level: LogLevel, thing: &dyn Display) {
    unsafe {
        raw::logk_prefix(level as u32);
    };
    let mut writer = LogWriter {};
    let mut fmt = Formatter::new(&mut writer, Default::default());
    let _ = thing.fmt(&mut fmt);
    unsafe {
        raw::rawprint(c"\x1b[0m\n".as_ptr());
    }
}

/// Print a formatted message.
pub fn logkf(level: LogLevel, thing: &dyn Display) {
    let acq = unsafe { raw::mutex_acquire(&raw mut raw::log_mtx, raw::LOG_MUTEX_TIMEOUT.into()) };
    logkf_unlocked(level, thing);
    if acq {
        unsafe { raw::mutex_release(&raw mut raw::log_mtx) };
    }
}

/// Print a formatted message without locking the mutex.
macro_rules! logkf_unlocked {
    ($level: expr, $($args:expr),*) => {
        logkf_unlocked($level, &format_args!($($args),*));
    };
}

/// Print a formatted message.
macro_rules! logkf {
    ($level: expr, $($args:expr),*) => {
        logkf($level, &format_args!($($args),*));
    };
}
