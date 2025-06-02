use core::{error::Error, fmt::Display, str};

use alloc::alloc::AllocError;

use super::raw;

/// Signal enum that matches those of BadgerOS.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Signal {
    SIGHUP = raw::SIGHUP,
    SIGINT = raw::SIGINT,
    SIGQUIT = raw::SIGQUIT,
    SIGILL = raw::SIGILL,
    SIGTRAP = raw::SIGTRAP,
    SIGABRT = raw::SIGABRT,
    SIGBUS = raw::SIGBUS,
    SIGFPE = raw::SIGFPE,
    SIGKILL = raw::SIGKILL,
    SIGUSR1 = raw::SIGUSR1,
    SIGSEGV = raw::SIGSEGV,
    SIGUSR2 = raw::SIGUSR2,
    SIGPIPE = raw::SIGPIPE,
    SIGALRM = raw::SIGALRM,
    SIGTERM = raw::SIGTERM,
    SIGSTKFLT = raw::SIGSTKFLT,
    SIGCHLD = raw::SIGCHLD,
    SIGCONT = raw::SIGCONT,
    SIGSTOP = raw::SIGSTOP,
    SIGTSTP = raw::SIGTSTP,
    SIGTTIN = raw::SIGTTIN,
    SIGTTOU = raw::SIGTTOU,
    SIGURG = raw::SIGURG,
    SIGXCPU = raw::SIGXCPU,
    SIGXFSZ = raw::SIGXFSZ,
    SIGVTALRM = raw::SIGVTALRM,
    SIGPROF = raw::SIGPROF,
    SIGWINCH = raw::SIGWINCH,
    SIGIO = raw::SIGIO,
    SIGPWR = raw::SIGPWR,
    SIGSYS = raw::SIGSYS,
}

/// Represents an error that should be raised as a signal to the process that invoked the syscall.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SignalError {
    pub signal: Signal,
    pub cause: usize,
}

impl Display for SignalError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "Signal {} cause {:x}", self.signal as u32, self.cause)
    }
}

impl Error for SignalError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        None
    }

    fn description(&self) -> &str {
        "description() is deprecated; use Display"
    }

    fn cause(&self) -> Option<&dyn Error> {
        self.source()
    }

    fn provide<'a>(&'a self, _: &mut core::error::Request<'a>) {}
}

/// Errno enum that matches those of BadgerOS.
#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Errno {
    EPERM = raw::EPERM,
    ENOENT = raw::ENOENT,
    ESRCH = raw::ESRCH,
    EINTR = raw::EINTR,
    EIO = raw::EIO,
    ENXIO = raw::ENXIO,
    E2BIG = raw::E2BIG,
    ENOEXEC = raw::ENOEXEC,
    EBADF = raw::EBADF,
    ECHILD = raw::ECHILD,
    EAGAIN = raw::EAGAIN,
    ENOMEM = raw::ENOMEM,
    EACCES = raw::EACCES,
    EFAULT = raw::EFAULT,
    ENOTBLK = raw::ENOTBLK,
    EBUSY = raw::EBUSY,
    EEXIST = raw::EEXIST,
    EXDEV = raw::EXDEV,
    ENODEV = raw::ENODEV,
    ENOTDIR = raw::ENOTDIR,
    EISDIR = raw::EISDIR,
    EINVAL = raw::EINVAL,
    ENFILE = raw::ENFILE,
    EMFILE = raw::EMFILE,
    ENOTTY = raw::ENOTTY,
    ETXTBSY = raw::ETXTBSY,
    EFBIG = raw::EFBIG,
    ENOSPC = raw::ENOSPC,
    ESPIPE = raw::ESPIPE,
    EROFS = raw::EROFS,
    EMLINK = raw::EMLINK,
    EPIPE = raw::EPIPE,
    EDOM = raw::EDOM,
    ERANGE = raw::ERANGE,

    EDEADLK = raw::EDEADLK,
    ENAMETOOLONG = raw::ENAMETOOLONG,
    ENOLCK = raw::ENOLCK,

    ENOSYS = raw::ENOSYS,

    ENOTEMPTY = raw::ENOTEMPTY,
    ELOOP = raw::ELOOP,
    ENOMSG = raw::ENOMSG,
    EIDRM = raw::EIDRM,
    ECHRNG = raw::ECHRNG,
    EL2NSYNC = raw::EL2NSYNC,
    EL3HLT = raw::EL3HLT,
    EL3RST = raw::EL3RST,
    ELNRNG = raw::ELNRNG,
    EUNATCH = raw::EUNATCH,
    ENOCSI = raw::ENOCSI,
    EL2HLT = raw::EL2HLT,
    EBADE = raw::EBADE,
    EBADR = raw::EBADR,
    EXFULL = raw::EXFULL,
    ENOANO = raw::ENOANO,
    EBADRQC = raw::EBADRQC,
    EBADSLT = raw::EBADSLT,

    EBFONT = raw::EBFONT,
    ENOSTR = raw::ENOSTR,
    ENODATA = raw::ENODATA,
    ETIME = raw::ETIME,
    ENOSR = raw::ENOSR,
    ENONET = raw::ENONET,
    ENOPKG = raw::ENOPKG,
    EREMOTE = raw::EREMOTE,
    ENOLINK = raw::ENOLINK,
    EADV = raw::EADV,
    ESRMNT = raw::ESRMNT,
    ECOMM = raw::ECOMM,
    EPROTO = raw::EPROTO,
    EMULTIHOP = raw::EMULTIHOP,
    EDOTDOT = raw::EDOTDOT,
    EBADMSG = raw::EBADMSG,
    EOVERFLOW = raw::EOVERFLOW,
    ENOTUNIQ = raw::ENOTUNIQ,
    EBADFD = raw::EBADFD,
    EREMCHG = raw::EREMCHG,
    ELIBACC = raw::ELIBACC,
    ELIBBAD = raw::ELIBBAD,
    ELIBSCN = raw::ELIBSCN,
    ELIBMAX = raw::ELIBMAX,
    ELIBEXEC = raw::ELIBEXEC,
    EILSEQ = raw::EILSEQ,
    ERESTART = raw::ERESTART,
    ESTRPIPE = raw::ESTRPIPE,
    EUSERS = raw::EUSERS,
    ENOTSOCK = raw::ENOTSOCK,
    EDESTADDRREQ = raw::EDESTADDRREQ,
    EMSGSIZE = raw::EMSGSIZE,
    EPROTOTYPE = raw::EPROTOTYPE,
    ENOPROTOOPT = raw::ENOPROTOOPT,
    EPROTONOSUPPORT = raw::EPROTONOSUPPORT,
    ESOCKTNOSUPPORT = raw::ESOCKTNOSUPPORT,
    EOPNOTSUPP = raw::EOPNOTSUPP,
    EPFNOSUPPORT = raw::EPFNOSUPPORT,
    EAFNOSUPPORT = raw::EAFNOSUPPORT,
    EADDRINUSE = raw::EADDRINUSE,
    EADDRNOTAVAIL = raw::EADDRNOTAVAIL,
    ENETDOWN = raw::ENETDOWN,
    ENETUNREACH = raw::ENETUNREACH,
    ENETRESET = raw::ENETRESET,
    ECONNABORTED = raw::ECONNABORTED,
    ECONNRESET = raw::ECONNRESET,
    ENOBUFS = raw::ENOBUFS,
    EISCONN = raw::EISCONN,
    ENOTCONN = raw::ENOTCONN,
    ESHUTDOWN = raw::ESHUTDOWN,
    ETOOMANYREFS = raw::ETOOMANYREFS,
    ETIMEDOUT = raw::ETIMEDOUT,
    ECONNREFUSED = raw::ECONNREFUSED,
    EHOSTDOWN = raw::EHOSTDOWN,
    EHOSTUNREACH = raw::EHOSTUNREACH,
    EALREADY = raw::EALREADY,
    EINPROGRESS = raw::EINPROGRESS,
    ESTALE = raw::ESTALE,
    EUCLEAN = raw::EUCLEAN,
    ENOTNAM = raw::ENOTNAM,
    ENAVAIL = raw::ENAVAIL,
    EISNAM = raw::EISNAM,
    EREMOTEIO = raw::EREMOTEIO,
    EDQUOT = raw::EDQUOT,

    ENOMEDIUM = raw::ENOMEDIUM,
    EMEDIUMTYPE = raw::EMEDIUMTYPE,
    ECANCELED = raw::ECANCELED,
    ENOKEY = raw::ENOKEY,
    EKEYEXPIRED = raw::EKEYEXPIRED,
    EKEYREVOKED = raw::EKEYREVOKED,
    EKEYREJECTED = raw::EKEYREJECTED,

    EOWNERDEAD = raw::EOWNERDEAD,
    ENOTRECOVERABLE = raw::ENOTRECOVERABLE,
    ERFKILL = raw::ERFKILL,
    EHWPOISON = raw::EHWPOISON,
}

impl Errno {
    /// Get the name of this errno.
    pub fn name(&self) -> &'static str {
        unsafe {
            let cs = raw::errno_get_name(*self as i32);
            str::from_raw_parts(cs, raw::cstr_length(cs))
        }
    }
    /// Get a brief description of this errno.
    pub fn desc(&self) -> &'static str {
        unsafe {
            let cs = raw::errno_get_desc(*self as i32);
            str::from_raw_parts(cs, raw::cstr_length(cs))
        }
    }
}

/// Represents an error designated by errno.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ErrnoError {
    pub errno: Errno,
}

impl Display for ErrnoError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{} ({})", self.errno.name(), self.errno.desc())
    }
}

impl Error for ErrnoError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        None
    }

    fn description(&self) -> &str {
        "description() is deprecated; use Display"
    }

    fn cause(&self) -> Option<&dyn Error> {
        self.source()
    }

    fn provide<'a>(&'a self, _: &mut core::error::Request<'a>) {}
}

impl Into<ErrnoError> for AllocError {
    fn into(self) -> ErrnoError {
        ErrnoError {
            errno: Errno::ENOMEM,
        }
    }
}

macro_rules! errno {
    ($errno: tt) => {
        crate::bindings::error::ErrnoError {
            errno: crate::bindings::error::Errno::$errno,
        }
    };
}
