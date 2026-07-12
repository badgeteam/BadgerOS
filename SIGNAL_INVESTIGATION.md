# Signal handling / `kill -9 1` investigation — 2026-07-12

Investigation into: (1) why the signal-60 test in `ktest-init/main.c` caused a SIGSEGV,
and (2) why `kill -9 1` didn't appear to work. Findings below are ordered by how they
were uncovered; the root-cause fix is in the last "FIXED" section.

## Fixed

### `sigret` clobbered the restored `a0` register (the actual SIGSEGV cause)

**File:** `kernel/tools/sysgen.py` (generator), regenerates `kernel/src/process/syscall/mod.rs`.
Also `kernel/cpu/riscv64/src/thread.rs` (secondary bug, see below).

The generated syscall dispatcher (`dispatch()` in `syscall/mod.rs`) unconditionally ran
`regs.set_retval(retval)` after *every* syscall, including `sigret`. But `sigret`'s handler
(`exit_signal` in `kernel/cpu/riscv64/src/usermode.rs`) already fully restores the entire
interrupted register file — including `a0` — from the saved signal frame. The generic
retval-write then executed *after* that restore and clobbered `a0` back to `0` (sigret's own
"success" return code), corrupting whatever the interrupted user code legitimately had in `a0`.

This only manifested when a signal happened to interrupt code that was relying on `a0`
across the interrupted point (proven by instrumenting `enter_signal`/`exit_signal`: the full
32-register save/restore round-trip was bit-for-bit correct — the corruption happened strictly
*after* `exit_signal` returned, inside the generic dispatch epilogue). In the reproduction, the
interrupted code was mid-prologue of `frg::printf_format` in `libc.so`, which loads through `a0`
a few instructions in — landing on a null pointer and crashing with SIGSEGV.

**Fix:** `sysgen.py`'s `gen_rust_marshalling()` now special-cases `sigret` to `return` immediately
after calling its marshal function, bypassing `regs.set_retval()` entirely. Regenerated
`mod.rs` accordingly (do not hand-edit `mod.rs` — it's generated).

Verified via 5 consecutive `make qemu` runs after the fix: the signal-60 test consistently
completes (`Handled signal 60` printed, handler returns cleanly, process continues) with zero
SIGSEGV recurrences, versus recurring crashes before the fix (non-deterministic — only manifested
when a signal landed on code relying on `a0`, which depends on SMP scheduling timing).

### `FloatState::enable()` never sets `float_enable = true`

**File:** `kernel/cpu/riscv64/src/thread.rs`, `FloatState::enable()`.

The function's doc comment and its own `debug_assert!(!self.float_enable)` clearly intend a
one-time "first FP use" transition, but the function never actually sets `self.float_enable = true`
after doing the enable work. Since `save_state`/`load_state` are both gated on
`if !self.float_enable { return; }`, this silently made FP register save/restore a permanent
no-op for any thread that ever used floating point — FP state would never be preserved across
traps, signal delivery, or context switches.

Found while investigating the SIGSEGV above (a red herring for that specific bug — see below —
but a real, independent bug). Fixed by adding `self.float_enable = true;` at the end of `enable()`.

## Not fixed — flagged for follow-up, out of scope for this session

### `kill -9 1` still untested

With the `sigret` bug fixed, PID 1 now survives the signal-60 test and proceeds to fork+exec `ls`
successfully. However, `ls` (coreutils, via mlibc) calls `ioctl` (likely `TIOCGWINSZ` for terminal
size), which isn't implemented in `mlibc/sysdeps/badgeros` (`Ioctl` sysdep is unimplemented —
see `__ensure(Library function fails due to missing sysdep) failed` in the log). This aborts
*inside* `ls`'s process — but the immediately following event is a **kernel panic**, not just that
one process dying:

```
FATAL src/../cpu/riscv64/src/exception.rs:109:29: misaligned pointer dereference:
address must be a multiple of 0x8 but is 0xffffffffffffffff
```

This happens at the very top of `riscv_exception_handler`, dereferencing `CpuLocal::get()` —
meaning the per-hart pointer itself is garbage (`-1` as `usize`) on a fresh trap entry, unrelated
to page faults or signal delivery. It reproduced identically (same PC `0x20004441c0`, same panic
site) across every run once execution reaches that point.

This kernel panic halts the whole system before `ktest-init`'s `main()` ever reaches its
interactive `readline()` prompt, so `kill -9 1` has not yet been tested interactively — there's
no live prompt to type it at in an automated run. Two independent blockers stack here:

1. Missing `Ioctl` sysdep in `mlibc/sysdeps/badgeros` (affects any program that probes terminal
   size, e.g. most shells/coreutils tools).
2. The `CpuLocal` garbage-pointer kernel panic, which turns that one missing-syscall abort into
   a full system halt instead of just killing the offending process.

Per your direction, these were intentionally left uninvestigated this session to keep focus on
the signal bug. To actually exercise `kill -9 1`, either:
- Implement (or stub) the `Ioctl` sysdep so `ls`/shells don't abort, or
- Skip running `ls` in `ktest-init/main.c`'s `main()` so `it` reaches the interactive prompt
  directly, or
- Fix the `CpuLocal` panic so a missing-sysdep abort in one process doesn't take down the kernel.

### Stale mlibc build artifacts (already resolved by you mid-session)

Early in this investigation, a broken/stale mlibc build (traced to a meson subproject
configuration issue you fixed directly — `libsmarter:no_install` unknown option) was causing the
dynamic linker (`ld.so`) itself to crash during its own bootstrap, before `ktest-init`'s `main()`
ever ran. That's what the *very first* reproduction in this session actually showed — it looked
like "the signal causes a SIGSEGV" only because nothing got far enough to test signals at all.
No action needed here; noting it for the historical record since it shaped the early (wrong)
theories in this investigation.

## Method notes

- `make qemu` does not rebuild the image — always `make image` first (per `CLAUDE.md`).
- The kernel can't shut itself down; every `make qemu` run in this investigation used
  `timeout 25 make qemu` (log auto-saved to `log` via the Makefile's `tee`).
- `kernel/tools/address-filter.py` (used by `make qemu`'s log pipe) only symbolizes against the
  *kernel* ELF (`kernel/output/badger-os.elf`). For userspace addresses, use `addr2line`/`objdump`
  directly against the relevant binary in `build/sysroot/` (e.g. `build/sysroot/usr/lib/libc.so`
  for `libc.so`-mapped addresses) — cross-reference against the `mem_map` syscall trace to find
  each mapped object's load base.
- `make image` does not force mlibc (or other jinx packages) to rebuild from source-only changes;
  it only picked up my `ktest-init` edits reliably because that recipe always recompiles from
  scratch. For mlibc/kernel-adjacent C++ changes, use `make rebuild PACKAGES=mlibc` (or `cd build
  && ../jinx rebuild <pkg>`) to force a real rebuild — note this may hit unrelated meson
  subproject/config issues in this checkout, as it did once this session.
