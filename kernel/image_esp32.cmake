
# Convert the raw binary file into a ESP32 image file
add_custom_command(
    OUTPUT badger-os.bin
    COMMAND cp badger-os.elf badger-os.elf.patch
    COMMAND ${BADGER_OBJCOPY} -O binary badger-os.elf badger-os.nochecksum.bin
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/pack-image.py badger-os.nochecksum.bin badger-os.bin
    DEPENDS badger-os.elf
)

# Install the ESP32 image.
add_custom_target(badger-os.bin.target ALL DEPENDS badger-os.bin)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/badger-os.bin DESTINATION .)
