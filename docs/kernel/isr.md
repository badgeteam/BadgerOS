# Traps and interrupts
Traps and interrupts are low-level mechanisms of the CPU used for error conditions and events respectively. Traps can be illegal instruction, memory access fault, system calls, etc. while interrupts are generated by other hardware like timers, GPIO, SPI, I²C or inter-CPU communication.

Currently responsible: Julian (RobotMan2412).

## Index
- [Scope](#scope)
- [Dependents](#dependents)
- [Dependencies](#dependencies)
- [Data types](#data-types)
- [API](#api)


# Scope
The interrupt service routine, or ISR for short, handles these CPU events and converts them into events in the kernel. A limited amount of code can run inside an interrupt, with the exception of system calls, which are run on the kernel thread associated with the process.


# Dependents
## [Scheduler](./scheduler.md)
The scheduler calls `isr_ctx_switch_set` to inform the ISR which context to run next. This context can be kernel side (either kernel thread or kernel side of user thread) or user side (user side of user thread).


# Dependencies
## [Scheduler](./scheduler.md)
When a trap or interrupt happens, the ISR can call `sched_request_switch_from_isr` to ask the scheduler if a context switch is in order and if so, to which context.
When a system call happens, the ISR calls `sched_raise_from_isr` to "raise" the privileges to kernel mode and run specified kernel code in the thread.


# Data types
## isr_ctx_t
The ISR context is a CPU-specific data structure that stores the execution context for a side of a thread; kernel threads have one ISR contexts while user threads have one each for their kernel and user sides.

Aside from the CPU-specific fields, which should not be accessed by code that is not CPU-specific, there are two general fields:

### Field: `sched_thread_t *thread`
Pointer to owning `sched_thread_t`.
Used by the ISR itself to call the scheduler with the appropriate thread handle.

### Field: `bool is_kernel_thread`
Thread is a kernel thread.
If true, the thread is run in M-mode, otherwise it is run in U-mode.

M-mode (or kernel-mode) contexts are (run as) the kernel, inheriting all the privileges associated. U-mode (or user-mode) contexts are for user code and their privileges are strictly regulated by the kernel.

The scheduler uses this field to indicate in which privilege mode the context should be run.


# API
## `static inline isr_ctx_t *isr_ctx_get();`
Get the current [ISR context](#isr_ctx_t).

Used by the scheduler and error handling functions as a CPU-specific way to get information about the execution state.

## `static inline isr_ctx_t *isr_ctx_switch_get();`
Get the outstanding context switch target, if any.

Can be used to query which ISR context is set to be switched to. If NULL, the current context is kept when the ISR returns.

## `static inline void isr_ctx_switch_set(isr_ctx_t *switch_to);`
Set the context switch target to switch to before exiting the trap/interrupt handler.

Used by the scheduler in response to a call to `sched_request_switch_from_isr`.

## `void isr_ctx_dump(isr_ctx_t const *ctx);`
Print a register dump given isr_ctx_t.

Used by error handlers to print the execution state when either the kernel panics or a user thread raises an exception.

## `void kernel_cur_regs_dump();`
Print a register dump of the current registers.

Used by error handlers when the kernel panics.
