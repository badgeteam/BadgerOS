
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.10.0)

set(target_arch "rv64imafdc_zicsr_zifencei")
set(target_abi "lp64")

set(cpu_riscv_enable_riscv_intc true)
set(cpu_riscv_enable_sifive_plic true)
set(cpu_riscv_enable_mmu true)

set(port_src
    ${CMAKE_CURRENT_LIST_DIR}/src/hal/gpio.c
    ${CMAKE_CURRENT_LIST_DIR}/src/hal/i2c.c
    ${CMAKE_CURRENT_LIST_DIR}/src/hal/spi.c
    ${CMAKE_CURRENT_LIST_DIR}/src/hal/time.c
    ${CMAKE_CURRENT_LIST_DIR}/src/interrupt.c
    ${CMAKE_CURRENT_LIST_DIR}/src/memprotect.c
    ${CMAKE_CURRENT_LIST_DIR}/src/port.c
)
set(port_include
    ${CMAKE_CURRENT_LIST_DIR}/include
    lib/limine
)
set(port_link
    -T${CMAKE_CURRENT_LIST_DIR}/linker.ld
    -L${CMAKE_CURRENT_LIST_DIR}
)
set(port_flags)
