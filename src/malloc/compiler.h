#pragma once

#define FORCEINLINE __attribute__((always_inline)) static inline

__attribute__((always_inline)) static inline void intr_pause() {
    #if (defined(__x86_64__) || defined(__i386__))
    __builtin_ia32_pause();
    #elif (defined(__arm__))
    __asm__ volatile ("yield");
    #elif (defined(__aarch64__))
    __asm__ volatile ("yield");
    #elif (defined(__riscv))
    __builtin_riscv_pause();
    #else
    // For architectures where there's no known pause instruction, a no-op.
    #endif
}
