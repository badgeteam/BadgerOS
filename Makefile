
MAKEFLAGS += --silent
ARCH ?= riscv64
EFI_PART_SIZE ?= 4MiB
ROOT_PART_SIZE ?= 59MiB
PACKAGES ?= '*'


.PHONY: image
image:
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
	#        build/sysroot/boot (created later)
	mkdir -p build/sysroot/dev
	mkdir -p build/sysroot/tmp
	mkdir -p build/sysroot/mnt
	
	# Temporarily point sysroot's /boot to efiroot
	rm -df build/sysroot/boot
	ln -s ../efiroot build/sysroot/boot
	
	# Ask Jinx nicely to install everything
	cd build && ../jinx update '*'
	cd build && ../jinx install sysroot '*'
	
	# System should have an existant empty directory at /boot, so restore that
	rm build/sysroot/boot
	mkdir -p build/sysroot/boot


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

.PHONY: host-rebuild
host-rebuild: build/.jinx-parameters
	cd build && ../jinx host-rebuild $(PACKAGES)


.PHONY: clean
clean:
	rm -rf build
