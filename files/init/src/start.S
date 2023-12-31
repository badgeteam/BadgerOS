
# SPDX-License-Identifier: MIT

#include "syscall.h"

    .global __global_pointer$
    .global main



    .global _start
    .text
    .p2align 1
_start:
    # Set stack and global pointer.
    .option push
    .option norvc
    la sp, stack_top
    la gp, __global_pointer$
    .option pop
    
    # TODO: argc and argv
    
    # Jump to main function
    jal main
    
    # Exit the process if main returns
    mv s0, a0
    li a7, SYSCALL_SYS_SHUTDOWN
    li a0, 0
    ecall
    li a7, SYSCALL_SELF_EXIT
    mv a0, s0
    ecall



    # Main stack.
    .section ".bss"
stack_bottom:
    .zero 1024
stack_top:
