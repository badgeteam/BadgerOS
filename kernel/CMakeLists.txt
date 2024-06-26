
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.10.0)

set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

# Set the RAMFS root
if(NOT DEFINED BADGER_RAMFS_ROOT)
    set(BADGER_RAMFS_ROOT ${CMAKE_CURRENT_LIST_DIR}/root)
endif()

# Set the C compiler
include(../common/compiler.cmake)

# Include port-specific.
include(port/${BADGEROS_PORT}/CMakeLists.txt)
include(cpu/riscv/CMakeLists.txt)

set(common_compiler_flags
    -ffreestanding                     # We do not compile against an OS.
    -static -fno-pic                   # Disable PIC
    -march=${target_arch}              # Selects the target CPU.
    -mabi=${target_abi}                # Selects target ABI
    -nodefaultlibs -nostdlib           # Do not link any default libraries like libgcc or libc.
    -O2                                # Optimize the code.
    -ggdb                              # Generate debug information in default extended format.
    -Werror=return-type                # Error when a function doesn't return a value, but declares to do so.
    -Werror=implicit-fallthrough
    -Wall -Wextra                      # Ramp up warning level.
    -std=gnu11                         # We use the C11 standard
    -DBADGEROS_KERNEL                  # Tell the code we're building for the kernel
    -DBADGEROS_PORT_${BADGEROS_PORT}
    -DBADGEROS_PORT="${BADGEROS_PORT}"
    -DBADGEROS_MALLOC_DEBUG_LEVEL=3    # Malloc debug level set to WARN
    -DSOFTBIT                          # Turn on our emulated bit operations
    -fno-omit-frame-pointer            # Always use frame pointer
    -ffunction-sections
    -fno-stack-protector
    -fno-exceptions
)
if("${CMAKE_BUILD_TYPE}" MATCHES "Release")
message("Building in release mode")
else()
message("Building in debug mode")
set(common_compiler_flags ${common_compiler_flags}
    -fsanitize=undefined               # Adds sanitizer for undefined behaviour.
    -fsanitize-undefined-trap-on-error
)
endif()

# we must pass the same options to GCC and LD when using LTO, as the linker will actually do the codegen
add_compile_options(${common_compiler_flags} ${cpu_flags} ${port_flags})
add_link_options(${common_compiler_flags} ${cpu_link} ${port_link} -Wl,--gc-sections -nostartfiles)

# For IDE users.
set(CMAKE_EXPORT_COMPILE_COMMANDS true)

project(badgeros C ASM)

# Define executable file.
set(target badger-os.elf)
add_executable(${target}
    ${CMAKE_CURRENT_LIST_DIR}/build/fs_root.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/arrays.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/badge_err.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/badge_format_str.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/badge_strings.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/int_routines.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/list.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/log.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/mutex.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/num_to_str.c
    ${CMAKE_CURRENT_LIST_DIR}/src/badgelib/rawprint.c
    ${CMAKE_CURRENT_LIST_DIR}/src/blockdevice/blkdev_ram.c
    ${CMAKE_CURRENT_LIST_DIR}/src/blockdevice/blockdevice.c
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/filesystem.c
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/syscall_impl.c
    # ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/vfs_fat.c
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/vfs_ramfs.c
    ${CMAKE_CURRENT_LIST_DIR}/src/filesystem/vfs_internal.c
    ${CMAKE_CURRENT_LIST_DIR}/src/hal/syscall_impl.c
    ${CMAKE_CURRENT_LIST_DIR}/src/malloc/malloc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/malloc/static-buddy.c
    ${CMAKE_CURRENT_LIST_DIR}/src/malloc/slab-alloc.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/kbelfx.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/process.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/sighandler.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/syscall_impl.c
    ${CMAKE_CURRENT_LIST_DIR}/src/process/syscall_util.c
    ${CMAKE_CURRENT_LIST_DIR}/src/scheduler/scheduler.c
    ${CMAKE_CURRENT_LIST_DIR}/src/scheduler/syscall_impl.c
    ${CMAKE_CURRENT_LIST_DIR}/src/housekeeping.c
    ${CMAKE_CURRENT_LIST_DIR}/src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/src/syscall.c
    ${cpu_src}
    ${port_src}
)
target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/src/malloc
    ${CMAKE_CURRENT_LIST_DIR}/include/badgelib
    ${CMAKE_CURRENT_LIST_DIR}/../common/include
    ${CMAKE_CURRENT_LIST_DIR}/../common/badgelib
    ${cpu_include}
    ${port_include}
)

# Add libraries.
add_subdirectory(lib/kbelf)
target_compile_options(kbelf PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/include/loading/kbelf_pre.h
)
target_include_directories(kbelf PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/include/badgelib
    ${CMAKE_CURRENT_LIST_DIR}/../common/include
    ${CMAKE_CURRENT_LIST_DIR}/../common/badgelib
    ${cpu_include}
    ${port_include}
)
target_link_libraries(${target} PRIVATE kbelf)


# Convert the raw binary file into a ESP32 image file
add_custom_command(
    OUTPUT badger-os.bin
    COMMAND cp badger-os.elf badger-os.elf.patch
    COMMAND "${BADGER_OBJCOPY}" -O binary badger-os.elf badger-os.nochecksum.bin
    COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../tools/pack-image.py" badger-os.nochecksum.bin badger-os.bin
    DEPENDS badger-os.elf
)
# Proxy target to allow installation of the bin file
add_custom_target(badger-os.bin.target ALL DEPENDS badger-os.bin)

# Also build a dump file that is put into the
add_custom_target(
    badger-os.elf.disasm
    ALL
    COMMAND "${BADGER_OBJDUMP}" -Sd badger-os.elf > badger-os.elf.disasm
    DEPENDS badger-os.elf
)

# Declare which files are installed to the output directory:
install(TARGETS badger-os.elf DESTINATION .)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/badger-os.bin" DESTINATION .)

