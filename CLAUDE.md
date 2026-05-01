
This is a hobby operating system project called "BadgerOS".

# The goal
A 64-bit POSIX-compliant operating system with a monolithic kernel supporting RISC-V and later x86.
**Only RISC-V** is supported right now.
The kernel is written in C and Rust, and C is slowly being phased out and re-written in Rust.

# Most important files:
- `build` - collection of all build and temporary files
- `host-recipes` - cross-compilation toolchains
- `recipes` - userspace software for the operating system
- `mlibc` - submodule; sources for the C standard library
- `kernel` - submodule; sources for the kernel
- `ktest-init` - a temporary init program meant to test the userspace side of things (e.g. syscalls)
Note: In mlibc, our sysdeps live under `mlibc/sysdeps/badgeros`

# Kernel structural overview
- `badgelib` - misc kernel-specific C libraries; deprecated
- `bindings` - connecting legacy C subsystems to Rust
- `boot` - startup process pre-userspace
- `device` - device subsystem
    - `builtin_driver` - AHCI, NS16550A, /dev/zero, /dev/null, PCIe
    - `class` - specializations over the generic device
    - `dtb` - device tree blob parsing and interpretation
- `filesystem` - inode-focussed virtual filesystem layer
    - `ext2` - FS driver for ext2
    - `fatfs` - FS driver for FAT12, FAT16 and FAT32 with LFN support
    - `partition` - partition detection code
- `kernel` - core kernel runtime
    - `sched` - scheduler
    - `sync` - synchronization primitives, e.g. mutex
- `malloc` - legacy memory allocated; deprecated
- `mem` - memory management
    - `vmm` - UVM-style virtual memory management
    - `pmm` - Buddy-allocator-based physical memory management
- `process` - process subsystem and syscall dispatch

# Commands you are allowed to use:
- `make qemu` - start the virtual machine up (does not build the image)
- `make image` - build the filesystem image with the operating system installed
- `make sysroot` - collect the files for the distribution's root directory
- `make kernel` - compile just the kernel

**IMPORTANT about the emulator**:
The operating system **does not support shutdown yet**, exit by entering `^Ax`.
I also made this target save the entire log output to `log`.
