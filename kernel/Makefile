
# SPDX-License-Identifier: MIT

MAKEFLAGS         += --silent
IDF_PATH          ?= $(shell pwd)/../esp-idf
SHELL             := /usr/bin/env bash
PORT              ?= $(shell find /dev/ -name ttyUSB* -or -name ttyACM* 2>/dev/null | head -1)
OUTPUT            ?= $(shell pwd)/firmware
BUILDDIR          ?= build
BADGER_RAMFS_ROOT ?= ../files/root
BADGEROS_PORT     ?= esp32c6

.PHONY: all clean configure build flash monitor clang-format-check clang-tidy-check openocd gdb

all: build flash monitor

configure:
	mkdir -p '$(BUILDDIR)'
	touch '$(BUILDDIR)/fs_root.c'
	cmake -B '$(BUILDDIR)' "-DCMAKE_BUILD_TYPE=$${CMAKE_BUILD_TYPE:-Debug}"

build: configure
	../tools/ramfs-gen.py '$(BADGER_RAMFS_ROOT)' '$(BUILDDIR)/fs_root.c' init_ramfs
	cmake --build '$(BUILDDIR)'
	cmake --install '$(BUILDDIR)' --prefix '$(OUTPUT)'

clang-format-check: configure
	echo "clang-format check the following files:"
	jq -r '.[].file' build/compile_commands.json | grep '\.[ch]$$'
	echo "analysis results:"
	clang-format-18 --dry-run $(shell jq -r '.[].file' build/compile_commands.json | grep '\.[ch]$$')

clang-tidy-check: configure
	echo "clang-tidy check the following files:"
	jq -r '.[].file' build/compile_commands.json | grep '\.[ch]$$'
	echo "analysis results:"
	clang-tidy-18 -p build $(shell jq -r '.[].file' build/compile_commands.json | grep '\.[ch]$$') --warnings-as-errors="*"

openocd:
	openocd -c 'set ESP_RTOS "none"' -f board/$(BADGEROS_PORT)-builtin.cfg

gdb:
	riscv32-unknown-elf-gdb -x port/$(BADGEROS_PORT)/gdbinit build/badger-os.elf

clean:
	rm -rf "$(BUILDDIR)"

flash: build
	./port/$(BADGEROS_PORT)/flash.sh '$(PORT)' '$(OUTPUT)'

monitor:
	echo -e "\033[1mType ^A^X to exit.\033[0m"
	picocom -q -b 115200 '$(PORT)' | ../tools/address-filter.py '$(OUTPUT)/badger-os.elf'; echo -e '\033[0m'
