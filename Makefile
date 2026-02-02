
MAKEFLAGS += --silent
ARCH ?= riscv64
EFI_PART_SIZE ?= 4MiB
ROOT_PART_SIZE ?= 251MiB
PACKAGES ?= libgcc mlibc-headers mlibc ktest-init


.PHONY: image
image: sysroot
	mkdir -p build/image
	./scripts/make_fatfs.sh $(EFI_PART_SIZE) build/efiroot build/image/efi.fatfs
	./scripts/make_e2fs.sh $(ROOT_PART_SIZE) build/sysroot build/image/root.e2fs
	./scripts/make_image.sh \
		build/image.hdd \
		'EFI partition'  boot build/image/efi.fatfs 0x0700 \
		'Root partition' root build/image/root.e2fs 0x8300

.PHONY: sysroot
sysroot: build/.jinx-parameters
	# EFI root folders
	mkdir -p build/efiroot/EFI/BOOT
	
	# System root folders
	#        build/sysroot/boot (created later)\
	mkdir -p build/sysroot/dev
	mkdir -p build/sysroot/tmp
	mkdir -p build/sysroot/mnt
	
	# Temporarily point sysroot's /boot to efiroot
	rm -df build/sysroot/boot
	ln -s ../efiroot build/sysroot/boot
	
	# Ask Jinx nicely to install everything
	cd build && ../jinx update $(PACKAGES)
	cd build && ../jinx reinstall sysroot $(PACKAGES)
	
	# System should have an existant empty directory at /boot, so restore that
	rm build/sysroot/boot
	mkdir -p build/sysroot/boot

.PHONY: clean-image
clean-image:
	rm -rf build/sysroot build/efiroot build/image build/image.hdd


.PHONY: qemu
qemu:
	qemu-system-riscv64 -s \
		-M virt,acpi=off -cpu rv64,sv48=false -smp 4 -m 1G \
		-device pcie-root-port,bus=pcie.0,id=pcisw0 \
		-device qemu-xhci,bus=pcisw0 -device usb-kbd \
		-drive if=pflash,unit=0,format=raw,file=$$HOME/the_projects/badgeros-kernel/kernel/build/cache/OVMF_RISCV64.fd \
		-drive if=none,id=hd0,format=raw,file=build/image.hdd,cache=none \
		-device ahci,id=achi0 \
		-device ide-hd,drive=hd0,bus=achi0.0 \
		-serial mon:stdio -nographic \
	| $$HOME/the_projects/badgeros-kernel/tools/address-filter.py -L -A riscv64-linux-gnu-addr2line \
		$$HOME/the_projects/badgeros-kernel/kernel/output/badger-os.elf


build/.jinx-parameters:
	mkdir -p build
	cd build && ../jinx init .. ARCH=$(ARCH)

.PHONY: build
build: build/.jinx-parameters
	cd build && ../jinx build $(PACKAGES)

.PHONY: host-build
host-build: build/.jinx-parameters
	cd build && ../jinx host-build $(PACKAGES)

.PHONY: rebuild
rebuild: build/.jinx-parameters
	cd build && ../jinx rebuild $(PACKAGES)

.PHONY: regenerate
regenerate: build/.jinx-parameters
	cd build && ../jinx regenerate $(PACKAGES)

.PHONY: host-rebuild
host-rebuild: build/.jinx-parameters
	cd build && ../jinx host-rebuild $(PACKAGES)

.PHONY: clean
clean:
	rm -rf build
