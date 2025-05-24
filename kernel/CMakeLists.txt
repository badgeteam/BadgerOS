
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.13.0)
cmake_policy(VERSION 3.13.0)

# For IDE users.
set(CMAKE_EXPORT_COMPILE_COMMANDS true)

set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

# Set the RAMFS root
if(NOT DEFINED BADGER_RAMFS_ROOT)
    set(BADGER_RAMFS_ROOT ${CMAKE_CURRENT_LIST_DIR}/root)
endif()



# Include the config.
include(../.config/config.cmake)
include(config_helper.cmake)
set(CMAKE_C_COMPILER ${CONFIG_COMPILER})
set(CONFIG_TARGET generic)

# Include port-specific.
include(port/${CONFIG_TARGET}/CMakeLists.txt)
include(cpu/${CONFIG_CPU}/CMakeLists.txt)



# Define options.
if("${CONFIG_CPU}" STREQUAL "riscv32")
    config_const(BOOT_PROTOCOL "esp32" "Boot protocol")
    config_enum(
        SOC "esp32p4" "Choose chipset"
        "esp32p4" "esp32c6"
    )
    config_const(ENABLE_ACPI false "Enable ACPI support")
else()
    config_const(BOOT_PROTOCOL "limine" "Boot protocol")
    config_const(SOC "generic" "Boot protocol")
    config_bool(ENABLE_ACPI true "Enable ACPI support")
endif()

config_enum(
    STACK_SIZE 16384 "Kernel stack size"
    8192 16384 32768 65536
)

if("${CONFIG_BOOT_PROTOCOL}" STREQUAL "limine" AND "${CONFIG_CPU}" STREQUAL "riscv64")
    config_bool(EMBED_UBOOT true "Embed U-boot")
else()
    config_const(EMBED_UBOOT false "Embed U-boot")
endif()

config_const(NOMMU false "Build for system without an MMU")
config_const(PAGE_SIZE 4096 "MMU / memory map page size")



set(common_compiler_flags
    -ffreestanding                     # We do not compile against an OS.
    -march=${CONFIG_KISA_SPEC}         # Selects the target CPU.
    -mabi=${CONFIG_KABI_SPEC}          # Selects target ABI
    -nodefaultlibs                     # Do not link any default libraries like libgcc or libc.
    -O2                                # Optimize the code.
    -ggdb -gdwarf-2                    # Generate debug information in default extended format.
    -Werror=return-type                # Error when a function doesn't return a value, but declares to do so.
    -Werror=implicit-fallthrough
    -Werror=int-conversion
    -Werror=incompatible-pointer-types
    -Werror=implicit-function-declaration
    -Wall -Wextra                      # Ramp up warning level.
    -Wno-missing-braces
    -std=gnu11                         # We use the C11 standard
    -DBADGEROS_KERNEL                  # Tell the code we're building for the kernel
    -DBADGEROS_MALLOC_DEBUG_LEVEL=2    # Malloc debug level set to WARN
    -DSOFTBIT                          # Turn on our emulated bit operations
    -fno-omit-frame-pointer            # Always use frame pointer
    -static
    -ffunction-sections
    -fno-stack-protector
    -fno-exceptions
    -Wno-unused-parameter
)
if("${CMAKE_BUILD_TYPE}" MATCHES "Release")
message("Building in release mode")
else()
message("Building in debug mode")
endif()

# we must pass the same options to GCC and LD when using LTO, as the linker will actually do the codegen
add_compile_options(${common_compiler_flags} ${cpu_flags} ${port_flags})
add_link_options(
    ${common_compiler_flags} ${cpu_flags} ${port_flags}
    ${cpu_link} ${port_link}
    -Wl,--gc-sections -Wl,--build-id=none -nodefaultlibs -nostartfiles
    -T${CMAKE_CURRENT_LIST_DIR}/misc/ld/${CONFIG_SOC}_${CONFIG_CPU}.ld
)



# Define executable.
project(badgeros C ASM)
set(target badger-os.elf)

set(kernel_src
    ${CMAKE_CURRENT_LIST_DIR}/build/fs_root.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/badge_format_str.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/fifo.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/hash.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/log.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/map.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/mutex.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/num_to_str.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/panic.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/radixtree.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/rawprint.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/semaphore.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/set.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/spinlock.c
    
    ${CMAKE_CURRENT_LIST_DIR}/src/blockdevice/blkdev_ram.c
    ${CMAKE_CURRENT_LIST_DIR}/src/blockdevice/blockdevice.c
    
    ${CMAKE_CURRENT_LIST_DIR}/src/bootp/${CONFIG_BOOT_PROTOCOL}.c
    
    ${CMAKE_CURRENT_LIST_DIR}/src/device/builtin_driver/pcie.c
    ${CMAKE_CURRENT_LIST_DIR}/src/device/class/block.c
    ${CMAKE_CURRENT_LIST_DIR}/src/device/class/pcictl.c
    ${CMAKE_CURRENT_LIST_DIR}/src/device/dtb/dtb.c
    ${CMAKE_CURRENT_LIST_DIR}/src/device/dtb/dtparse.c
    ${CMAKE_CURRENT_LIST_DIR}/src/device/builtin_drivers.c
    ${CMAKE_CURRENT_LIST_DIR}/src/device/device.c
    
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/filesystem.c
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/syscall_impl.c
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/fs_fat.c
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/fs_ramfs.c
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/vfs_fifo.c
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/vfs_internal.c
    ${CMAKE_CURRENT_LIST_DIR}/src/freestanding/int_routines.c
    ${CMAKE_CURRENT_LIST_DIR}/src/freestanding/string.c
    
    ${CMAKE_CURRENT_LIST_DIR}/src/malloc/malloc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/malloc/static-buddy.c
    ${CMAKE_CURRENT_LIST_DIR}/src/malloc/slab-alloc.c
    
    ${CMAKE_CURRENT_LIST_DIR}/src/process/kbelfx.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/proc_memmap.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/process.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/sighandler.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/syscall_impl.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/syscall_util.c
    
    ${CMAKE_CURRENT_LIST_DIR}/src/scheduler/scheduler.c
    ${CMAKE_CURRENT_LIST_DIR}/src/scheduler/waitlist.c
    
    ${CMAKE_CURRENT_LIST_DIR}/src/cache.c
    ${CMAKE_CURRENT_LIST_DIR}/src/housekeeping.c
    ${CMAKE_CURRENT_LIST_DIR}/src/interrupt.c
    ${CMAKE_CURRENT_LIST_DIR}/src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/src/memprotect.c
    ${CMAKE_CURRENT_LIST_DIR}/src/page_alloc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/smp.c
    ${CMAKE_CURRENT_LIST_DIR}/src/syscall.c
    ${CMAKE_CURRENT_LIST_DIR}/src/time.c
)
set(kernel_include
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/include/badgelib
    ${CMAKE_CURRENT_LIST_DIR}/../common/include
    ${CMAKE_CURRENT_LIST_DIR}/../common/badgelib
    ${CMAKE_CURRENT_LIST_DIR}/../.config
)

if(${CONFIG_ENABLE_ACPI})
    include(lib/uacpi/uacpi.cmake)
    set(kernel_src ${kernel_src}
        ${CMAKE_CURRENT_LIST_DIR}/src/device/uacpi_kernel_api.c
        ${UACPI_SOURCES}
    )
    set(kernel_include ${kernel_include} ${UACPI_INCLUDES})
endif()

add_executable(${target}
    ${kernel_src}
    ${cpu_src}
    ${port_src}
)
include_directories(
    ${kernel_include}
    ${cpu_include}
    ${port_include}
)
include_directories(SYSTEM ${port_system_include})

# Add libraries.
add_subdirectory(../common/badgelib badgelib)
add_subdirectory(lib/kbelf)
target_compile_options(kbelf PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/include/loading/kbelf_pre.h
)
target_link_libraries(kbelf PRIVATE badgelib)
target_link_libraries(${target} PRIVATE kbelf badgelib)



# Copy the BadgerOS binary to the output directory.
install(TARGETS badger-os.elf DESTINATION .)

# Create a disassembled version for easier debugging.
add_custom_target(
    badger-os.elf.disasm
    ALL
    COMMAND ${CONFIG_PREFIX}objdump -Sd badger-os.elf > badger-os.elf.disasm
    DEPENDS badger-os.elf
)

if("${CONFIG_BOOT_PROTOCOL}" STREQUAL "esp32")
    include(image_esp32.cmake)
elseif("${CONFIG_BOOT_PROTOCOL}" STREQUAL "limine")
    include(image_limine.cmake)
endif()
