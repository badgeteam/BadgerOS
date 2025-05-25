
// SPDX-License-Identifier: MIT

#pragma once

#if BADGEROS_KERNEL
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Get the name of an ERRNO.
char const *errno_get_name(int errno);
// Get a brief description of an errno.
char const *errno_get_desc(int errno);
// Indicates that a function returns an errno.
typedef struct {
    int errno;
} errno_t;
// Indicates that a function returns a size_t or errno.
typedef struct {
    intptr_t errno;
} errno_size_t;
// Indicates that a function returns a pointer or an errno.
typedef struct {
    void *ptr;
    int   errno;
} errno_ptr_t;
#endif



// Operation not permitted.
#define EPERM   1
// No such file or directory.
#define ENOENT  2
// No such process.
#define ESRCH   3
// Interrupted system call.
#define EINTR   4
// I/O error.
#define EIO     5
// No such device or address.
#define ENXIO   6
// Argument list too long.
#define E2BIG   7
// Exec format error.
#define ENOEXEC 8
// Bad file number.
#define EBADF   9
// No child processes.
#define ECHILD  10
// Try again.
#define EAGAIN  11
// Out of memory.
#define ENOMEM  12
// Permission denied.
#define EACCES  13
// Bad address.
#define EFAULT  14
// Block device required.
#define ENOTBLK 15
// Device or resource busy.
#define EBUSY   16
// File exists.
#define EEXIST  17
// Cross-device link.
#define EXDEV   18
// No such device.
#define ENODEV  19
// Not a directory.
#define ENOTDIR 20
// Is a directory.
#define EISDIR  21
// Invalid argument.
#define EINVAL  22
// File table overflow.
#define ENFILE  23
// Too many open files.
#define EMFILE  24
// Not a typewriter.
#define ENOTTY  25
// Text file busy.
#define ETXTBSY 26
// File too large.
#define EFBIG   27
// No space left on device.
#define ENOSPC  28
// Illegal seek.
#define ESPIPE  29
// Read-only file system.
#define EROFS   30
// Too many links.
#define EMLINK  31
// Broken pipe.
#define EPIPE   32
// Math argument out of domain of func.
#define EDOM    33
// Math result not representable.
#define ERANGE  34

// Resource deadlock would occur.
#define EDEADLK      35
// File name too long.
#define ENAMETOOLONG 36
// No locks available.
#define ENOLCK       37

// Invalid system call number.
#define ENOSYS 38

// Directory not empty.
#define ENOTEMPTY   39
// Too many symbolic links encountered.
#define ELOOP       40
// Not supported.
#define ENOTSUP     41
// Operation would block.
#define EWOULDBLOCK EAGAIN
// No message of desired type.
#define ENOMSG      42
// Identifier removed.
#define EIDRM       43
// Channel number out of range.
#define ECHRNG      44
// Level 2 not synchronized.
#define EL2NSYNC    45
// Level 3 halted.
#define EL3HLT      46
// Level 3 reset.
#define EL3RST      47
// Link number out of range.
#define ELNRNG      48
// Protocol driver not attached.
#define EUNATCH     49
// No CSI structure available.
#define ENOCSI      50
// Level 2 halted.
#define EL2HLT      51
// Invalid exchange.
#define EBADE       52
// Invalid request descriptor.
#define EBADR       53
// Exchange full.
#define EXFULL      54
// No anode.
#define ENOANO      55
// Invalid request code.
#define EBADRQC     56
// Invalid slot.
#define EBADSLT     57

// Resource deadlock would occur.
#define EDEADLOCK EDEADLK

// Bad font file format.
#define EBFONT          59
// Device not a stream.
#define ENOSTR          60
// No data available.
#define ENODATA         61
// Timer expired.
#define ETIME           62
// Out of streams resources.
#define ENOSR           63
// Machine is not on the network.
#define ENONET          64
// Package not installed.
#define ENOPKG          65
// Object is remote.
#define EREMOTE         66
// Link has been severed.
#define ENOLINK         67
// Advertise error.
#define EADV            68
// Srmount error.
#define ESRMNT          69
// Communication error on send.
#define ECOMM           70
// Protocol error.
#define EPROTO          71
// Multihop attempted.
#define EMULTIHOP       72
// RFS specific error.
#define EDOTDOT         73
// Not a data message.
#define EBADMSG         74
// Value too large for defined data type.
#define EOVERFLOW       75
// Name not unique on network.
#define ENOTUNIQ        76
// File descriptor in bad state.
#define EBADFD          77
// Remote address changed.
#define EREMCHG         78
// Can not access a needed shared library.
#define ELIBACC         79
// Accessing a corrupted shared library.
#define ELIBBAD         80
// .lib section in a.out corrupted.
#define ELIBSCN         81
// Attempting to link in too many shared libraries.
#define ELIBMAX         82
// Cannot exec a shared library directly.
#define ELIBEXEC        83
// Illegal byte sequence.
#define EILSEQ          84
// Interrupted system call should be restarted.
#define ERESTART        85
// Streams pipe error.
#define ESTRPIPE        86
// Too many users.
#define EUSERS          87
// Socket operation on non-socket.
#define ENOTSOCK        88
// Destination address required.
#define EDESTADDRREQ    89
// Message too long.
#define EMSGSIZE        90
// Protocol wrong type for socket.
#define EPROTOTYPE      91
// Protocol not available.
#define ENOPROTOOPT     92
// Protocol not supported.
#define EPROTONOSUPPORT 93
// Socket type not supported.
#define ESOCKTNOSUPPORT 94
// Operation not supported on transport endpoint.
#define EOPNOTSUPP      95
// Protocol family not supported.
#define EPFNOSUPPORT    96
// Address family not supported by protocol.
#define EAFNOSUPPORT    97
// Address already in use.
#define EADDRINUSE      98
// Cannot assign requested address.
#define EADDRNOTAVAIL   99
// Network is down.
#define ENETDOWN        100
// Network is unreachable.
#define ENETUNREACH     101
// Network dropped connection because of reset.
#define ENETRESET       102
// Software caused connection abort.
#define ECONNABORTED    103
// Connection reset by peer.
#define ECONNRESET      104
// No buffer space available.
#define ENOBUFS         105
// Transport endpoint is already connected.
#define EISCONN         106
// Transport endpoint is not connected.
#define ENOTCONN        107
// Cannot send after transport endpoint shutdown.
#define ESHUTDOWN       108
// Too many references: cannot splice.
#define ETOOMANYREFS    109
// Connection (or other operation) timed out.
#define ETIMEDOUT       110
// Connection refused.
#define ECONNREFUSED    111
// Host is down.
#define EHOSTDOWN       112
// No route to host.
#define EHOSTUNREACH    113
// Operation already in progress.
#define EALREADY        114
// Operation now in progress.
#define EINPROGRESS     115
// Stale file handle.
#define ESTALE          116
// Structure needs cleaning.
#define EUCLEAN         117
// Not a XENIX named type file.
#define ENOTNAM         118
// No XENIX semaphores available.
#define ENAVAIL         119
// Is a named type file.
#define EISNAM          120
// Remote I/O error.
#define EREMOTEIO       121
// Quota exceeded.
#define EDQUOT          122

// No medium found.
#define ENOMEDIUM    123
// Wrong medium type.
#define EMEDIUMTYPE  124
// Operation Canceled.
#define ECANCELED    125
// Required key not available.
#define ENOKEY       126
// Key has expired.
#define EKEYEXPIRED  127
// Key has been revoked.
#define EKEYREVOKED  128
// Key was rejected by service.
#define EKEYREJECTED 129

// Owner died.
#define EOWNERDEAD      130
// State not recoverable.
#define ENOTRECOVERABLE 131
// Operation not possible due to RF-kill.
#define ERFKILL         132
// Memory page has hardware error.
#define EHWPOISON       133
