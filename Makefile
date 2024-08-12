
# SPDX-License-Identifier: MIT

MAKEFLAGS += --silent
SHELL    := /usr/bin/env bash

.PHONY: all
all: build

.PHONY: config
configure: config
config:
	./tools/config.py
	python -m venv .venv
	.venv/bin/pip install esptool
	git submodule update --init

.PHONY: hh24_defconfig
hh24_defconfig:
	./tools/config.py --target esp32c6 --use-default

.PHONY: why2025_defconfig
why2025_defconfig:
	./tools/config.py --target esp32p4 --use-default

.PHONY: unmatched_defconfig
unmatched_defconfig:
	./tools/config.py --target generic --use-default --vec_spec none

.PHONY: build
build:
	$(MAKE) -C files build
	$(MAKE) -C kernel build

.PHONY: clean
clean:
	$(MAKE) -C files clean
	$(MAKE) -C kernel clean

.PHONY: cmake-configure
cmake-configure:
	$(MAKE) -C files build
	$(MAKE) -C kernel cmake-configure

.PHONY: openocd
openocd:
	$(MAKE) -C kernel openocd

.PHONY: gdb
gdb:
	$(MAKE) -C kernel gdb

.PHONY: qemu
qemu:
	$(MAKE) -C files build
	$(MAKE) -C kernel qemu

.PHONY: flash
flash:
	$(MAKE) -C files build
	$(MAKE) -C kernel flash

.PHONY: image
image:
	$(MAKE) -C files build
	$(MAKE) -C kernel image

.PHONY: monitor
monitor:
	$(MAKE) -C kernel monitor

.PHONY: clang-format-check
clang-format-check:
	$(MAKE) -C kernel clang-format-check

.PHONY: clang-tidy-check
clang-tidy-check:
	$(MAKE) -C kernel clang-tidy-check
