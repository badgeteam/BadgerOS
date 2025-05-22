
# Limine sources.
if("${CONFIG_CPU}" STREQUAL "riscv64")
set(limine_efi ${CMAKE_CURRENT_LIST_DIR}/lib/limine/BOOTRISCV64.EFI)
elseif("${CONFIG_CPU}" STREQUAL "x86_64")
set(limine_efi ${CMAKE_CURRENT_LIST_DIR}/lib/limine/BOOTX64.EFI)
endif()
set(limine_conf ${CMAKE_CURRENT_LIST_DIR}/limine.conf)

# Custom target that collects the files for the boot partition.
set(boot_dir ${CMAKE_BINARY_DIR}/boot.dir)
add_custom_command(
    OUTPUT ${boot_dir}
    COMMAND rm -rf ${boot_dir}
    COMMAND mkdir -p ${boot_dir}/EFI/BOOT
    COMMAND mkdir -p ${boot_dir}/boot
    COMMAND cp ${limine_efi} ${boot_dir}/EFI/BOOT/
    COMMAND cp ${limine_conf} ${boot_dir}/boot/
    COMMAND cp ${CMAKE_BINARY_DIR}/badger-os.elf ${boot_dir}/boot/
    DEPENDS badger-os.elf ${limine_efi} ${limine_conf}
)
add_custom_target(boot.dir.target DEPENDS ${boot_dir})

# Custom target that creates an appropriate FAT filesystem.
set(boot_fatfs ${CMAKE_BINARY_DIR}/boot.fatfs)
add_custom_command(
    OUTPUT ${boot_fatfs}
    COMMAND rm -f ${boot_fatfs}
    COMMAND dd if=/dev/zero bs=1M count=4 of=${boot_fatfs}
    COMMAND mformat -i ${boot_fatfs}
    COMMAND mcopy -s -i ${boot_fatfs} ${boot_dir}/* ::/
    DEPENDS boot.dir.target
)
add_custom_target(boot.fatfs.target DEPENDS ${boot_fatfs})

# Custom target for building the image.
set(image_iso ${CMAKE_BINARY_DIR}/image.iso)
if(CONFIG_EMBED_UBOOT)
    # Variant with U-boot embedded.
    add_custom_target(
        uboot.target
        make -C ${CMAKE_CURRENT_LIST_DIR}/lib/opensbi PLATFORM=generic CROSS_COMPILE=${CONFIG_PREFIX}
        COMMAND make -C ${CMAKE_CURRENT_LIST_DIR}/lib/u-boot sifive_unmatched_defconfig CROSS_COMPILE=${CONFIG_PREFIX}
        COMMAND make -C ${CMAKE_CURRENT_LIST_DIR}/lib/u-boot OPENSBI=${CMAKE_CURRENT_LIST_DIR}/lib/opensbi/build/platform/generic/firmware/fw_dynamic.bin CROSS_COMPILE=${CONFIG_PREFIX}
    )
    
    add_custom_command(
        OUTPUT ${image_iso}
        COMMAND rm -f ${image_iso}
        COMMAND dd if=/dev/zero bs=1M count=64 of=${image_iso}
        COMMAND
            sgdisk -a 1
                --new=1:34:2081    --change-name=1:spl   --typecode=1:5B193300-FC78-40CD-8002-E86C45580B47
                --new=2:2082:4095  --change-name=2:uboot --typecode=2:2E54B353-1271-4842-806F-E436D6AF6985
                --new=3:4096:12287 --change-name=3:boot  --typecode=3:0x0700
                --new=4:12288:-0   --change-name=4:root  --typecode=4:0x8300
                ${image_iso}
        COMMAND dd if=${CMAKE_CURRENT_LIST_DIR}/lib/u-boot/spl/u-boot-spl.bin bs=512 seek=34   of=${image_iso} conv=notrunc
        COMMAND dd if=${CMAKE_CURRENT_LIST_DIR}/lib/u-boot/u-boot.itb         bs=512 seek=2082 of=${image_iso} conv=notrunc
        COMMAND dd if=${boot_fatfs}                                           bs=512 seek=4096 of=${image_iso} conv=notrunc
        DEPENDS badger-os.elf boot.fatfs.target uboot.target   
    )
else()
    # Variant without U-boot embedded.
endif()
add_custom_target(image.iso.target ALL DEPENDS ${image_iso})

