
MAKEFLAGS += --silent
ARCH ?= riscv64
STAGE2_SIZE ?= 2MiB
EFI_PART_SIZE ?= 4MiB
ROOT_PART_SIZE ?= 503MiB
PACKAGES ?= limine kernel libgcc mlibc-headers mlibc ktest-init coreutils bash
EXE ?= bin/bash
SMP ?= 2
MEM ?= 2G
QEMU ?= qemu-system-$(ARCH)
QEMUFLAGS ?=
LIMINE ?= $(BUILD)/builds/limine/limine
BUILD ?= ./build/$(ARCH)
IMAGE ?= image-$(ARCH).hdd
JINX ?= $(shell realpath jinx)
KERNEL = $(BUILD)/builds/kernel/output/kernel

ifeq "$(ARCH)" "x86_64"
MACHINE ?= -M q35,smm=off
endif
ifeq "$(ARCH)" "riscv64"
MACHINE ?= -M virt,acpi=off -cpu rv64
endif

ifeq "$(ARCH)" "x86_64"
TOOLCHAIN ?=
else
TOOLCHAIN ?= $(ARCH)-linux-gnu-
endif



.PHONY: image
image: sysroot
	# Temporarily move /boot out to make FS images
	rm -rf $(BUILD)/efiroot
	mv $(BUILD)/sysroot/boot $(BUILD)/efiroot
	mkdir $(BUILD)/sysroot/boot
	
	mkdir -p $(BUILD)/image
	dd if=/dev/null of=$(BUILD)/image/stage2 bs=1 seek=$(STAGE2_SIZE)
	./scripts/make_fatfs.sh $(EFI_PART_SIZE) $(BUILD)/efiroot $(BUILD)/image/efi.fatfs
	./scripts/make_e2fs.sh $(ROOT_PART_SIZE) $(BUILD)/sysroot $(BUILD)/image/root.e2fs
	./scripts/make_image.sh \
		$(IMAGE) \
		'BIOS stage2'	 bios $(BUILD)/image/stage2    0xef02 \
		'EFI partition'  boot $(BUILD)/image/efi.fatfs 0x0700 \
		'Root partition' root $(BUILD)/image/root.e2fs 0x8300
	
ifeq "$(ARCH)" "x86_64"
	$(LIMINE) bios-install $(IMAGE)
endif
	
	# Restore /boot
	rmdir $(BUILD)/sysroot/boot
	mv $(BUILD)/efiroot $(BUILD)/sysroot/boot

.PHONY: sysroot
sysroot: $(BUILD)/.jinx-parameters
	mkdir -p $(BUILD)/sysroot/boot
	mkdir -p $(BUILD)/sysroot/dev
	mkdir -p $(BUILD)/sysroot/tmp
	mkdir -p $(BUILD)/sysroot/mnt
	mkdir -p $(BUILD)/sysroot/usr/lib
	mkdir -p $(BUILD)/sysroot/usr/bin
	mkdir -p $(BUILD)/sysroot/usr/sbin
	ln -snTf usr/lib  $(BUILD)/sysroot/lib
	ln -snTf usr/bin  $(BUILD)/sysroot/bin
	ln -snTf usr/sbin $(BUILD)/sysroot/sbin
	
	# Ask Jinx nicely to install everything
	cd $(BUILD) && $(JINX) update $(PACKAGES)
	cd $(BUILD) && $(JINX) reinstall sysroot $(PACKAGES)

.PHONY: clean-image
clean-image:
	rm -rf $(BUILD)/sysroot $(BUILD)/efiroot $(BUILD)/image $(IMAGE)


.PHONY: qemu
qemu: edk2-ovmf
	$(QEMU) $(QEMUFLAGS) -s \
		$(MACHINE) -smp $(SMP) -m $(MEM) \
		-device pcie-root-port,bus=pcie.0,id=pcisw0 \
		-device qemu-xhci,bus=pcisw0 -device usb-kbd \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-drive if=none,id=hd0,format=raw,file=$(IMAGE),cache=none \
		-device ahci,id=achi0 \
		-device ide-hd,drive=hd0,bus=achi0.0 \
		-serial mon:stdio -nographic
# 	| kernel/tools/address-filter.py -L -A $(TOOLCHAIN)addr2line $(KERNEL) \
# 	| tee log

edk2-ovmf:
	curl -L https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.gz | gunzip | tar -xf -

.PHONY: gdb
gdb:
	$(TOOLCHAIN)gdb $(KERNEL) -x gdbinit-k

.PHONY: user-gdb
user-gdb:
	$(TOOLCHAIN)gdb $(BUILD)/sysroot/$(EXE) -x gdbinit-u

.PHONY: ldso-gdb
ldso-gdb:
	$(TOOLCHAIN)gdb -x gdbinit-ldso


$(BUILD)/.jinx-parameters:
	mkdir -p $(BUILD)
	cd $(BUILD) && $(JINX) init ../.. ARCH=$(ARCH)

.PHONY: build
build: $(BUILD)/.jinx-parameters
	cd $(BUILD) && $(JINX) build $(PACKAGES)

.PHONY: host-build
host-build: $(BUILD)/.jinx-parameters
	cd $(BUILD) && $(JINX) host-build $(PACKAGES)

.PHONY: rebuild
rebuild: $(BUILD)/.jinx-parameters
	cd $(BUILD) && $(JINX) rebuild $(PACKAGES)

.PHONY: regenerate
regenerate: $(BUILD)/.jinx-parameters
	cd $(BUILD) && $(JINX) regenerate $(PACKAGES)

.PHONY: host-rebuild
host-rebuild: $(BUILD)/.jinx-parameters
	cd $(BUILD) && $(JINX) host-rebuild $(PACKAGES)

.PHONY: clean
clean:
	rm -rf build
