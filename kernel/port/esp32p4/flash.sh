
PORT="$1"
OUTPUT="$2"

../.venv/bin/python -m esptool -b 921600 --port "$PORT" \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB \
    0x2000 port/esp32p4/bootloader.bin \
    0x10000 "$OUTPUT/badger-os.bin" \
    0x8000 port/esp32p4/partition-table.bin
