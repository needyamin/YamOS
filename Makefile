# ============================================================================
#  YamKernel - Graph-Based Adaptive Operating System Kernel
#  Build System
# ============================================================================

KERNEL_NAME := yamkernel

# Toolchain (requires x86_64-elf cross-compiler or native Linux GCC)
CC      := x86_64-elf-gcc
AS      := nasm
LD      := x86_64-elf-ld
OBJCOPY := x86_64-elf-objcopy

# If cross-compiler not found, try native (for WSL/Linux)
ifeq ($(shell which $(CC) 2>/dev/null),)
    CC      := gcc
    LD      := ld
    OBJCOPY := objcopy
endif

HOST_CC := gcc

# Flags
CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-stack-check \
          -fno-pie -fno-pic -m64 -march=x86-64 -mno-80387 -mno-mmx \
          -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel \
          -Wall -Wextra -Werror -O2 -g \
          -Isrc/include -Isrc

KERNEL_CFLAGS := $(CFLAGS) -DYAM_KERNEL \
                 -Ithird_party/mbedtls/include

LDFLAGS := -nostdlib -static -T linker.ld -z max-page-size=0x1000

LIBGCC := $(shell $(CC) -print-libgcc-file-name 2>/dev/null)
ifeq ($(LIBGCC),)
    LIBGCC :=
endif

MBEDTLS_LIBS := third_party/mbedtls/library/libmbedcrypto.a \
                third_party/mbedtls/library/libmbedx509.a \
                third_party/mbedtls/library/libmbedtls.a

ASFLAGS := -f elf64

# Directories
SRC_DIR   := src
BUILD_DIR := build
ISO_DIR   := $(BUILD_DIR)/iso_root

# Embedded PEM for HTTPS trust store (xxd -i -> unsigned char isrgrootx1_pem[])
CA_EMBED_SRC := $(BUILD_DIR)/net/isrgrootx1_pem.c
CA_EMBED_OBJ := $(BUILD_DIR)/net/isrgrootx1_pem.o

# Source files (Exclude OS-level apps, drivers, and userspace libs from kernel build, keep services like compositor)
C_SRCS := $(shell find $(SRC_DIR) -name '*.c' -type f -not -path '$(SRC_DIR)/os/apps/*' -not -path '$(SRC_DIR)/os/drivers/*' -not -path '$(SRC_DIR)/os/lib/*' -not -path '$(SRC_DIR)/os/ports/*')
ASM_SRCS := $(shell find $(SRC_DIR) -name '*.asm' -type f -not -path '$(SRC_DIR)/os/apps/*' -not -path '$(SRC_DIR)/os/drivers/*' -not -path '$(SRC_DIR)/os/lib/*' -not -path '$(SRC_DIR)/os/ports/*')

# Object files
C_OBJS   := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
ASM_OBJS := $(patsubst $(SRC_DIR)/%.asm,$(BUILD_DIR)/%.asm.o,$(ASM_SRCS))
OBJS     := $(C_OBJS) $(ASM_OBJS)

# Final targets
KERNEL_ELF := $(BUILD_DIR)/$(KERNEL_NAME).elf
KERNEL_ISO := $(BUILD_DIR)/$(KERNEL_NAME).iso
DISK_IMG   := $(BUILD_DIR)/yamos-fat32.disk

# Splash screen images
IMG2RAW       := $(BUILD_DIR)/img2raw
LOGO_BIN      := $(BUILD_DIR)/logo.bin
WALLPAPER_BIN := $(BUILD_DIR)/wallpaper.bin

# User-space OS services
AUTHD_ELF   := $(BUILD_DIR)/authd.elf
HELLO_ELF   := $(BUILD_DIR)/hello.elf
EXEC_TEST_ELF := $(BUILD_DIR)/exec-test.elf
ORPHAN_TEST_ELF := $(BUILD_DIR)/orphan-test.elf
# User-space libc
LIBC_DIR := src/os/lib/libc
LIBC_SRCS := $(LIBC_DIR)/stdio.c $(LIBC_DIR)/stdlib.c $(LIBC_DIR)/string.c $(LIBC_DIR)/ctype.c $(LIBC_DIR)/posix.c $(LIBC_DIR)/time.c $(LIBC_DIR)/wchar.c $(LIBC_DIR)/pthread.c $(LIBC_DIR)/signal.c
LIBC_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(LIBC_SRCS))

USER_CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -fno-stack-check \
               -fno-pie -fno-pic -no-pie -static -m64 -march=x86-64 -msse2 -mfpmath=sse \
               -mno-red-zone -O2 -g \
               -Isrc -Isrc/include -Isrc/os/lib -Isrc/os/lib/libc

# ============================================================================
#  Targets
# ============================================================================

.PHONY: all clean run run-vmware run-serial run-serial-only verify-log debug iso setup

all: $(KERNEL_ELF)

iso: $(KERNEL_ISO)

# Link kernel
$(KERNEL_ELF): $(OBJS) $(CA_EMBED_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ -Wl,--start-group $(MBEDTLS_LIBS) -Wl,--end-group $(LIBGCC)
	@echo "[LINK] $@"

$(BUILD_DIR)/net/isrgrootx1_pem.c: src/net/certs/isrgrootx1.pem
	@mkdir -p $(dir $@)
	xxd -i $< | sed 's/src_net_certs_isrgrootx1_pem/isrgrootx1_pem/g' > $@

$(BUILD_DIR)/net/isrgrootx1_pem.o: $(BUILD_DIR)/net/isrgrootx1_pem.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@
	@echo "[EMBED] $<"

# Compile C sources (Kernel)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@
	@echo "[CC]   $<"

# Assemble NASM sources
$(BUILD_DIR)/%.asm.o: $(SRC_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@
	@echo "[ASM]  $<"

# Compile user-space libc with userland ABI flags, not kernel no-FPU flags.
$(BUILD_DIR)/os/lib/libc/%.o: $(SRC_DIR)/os/lib/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@
	@echo "[LIBC] $<"

# ============================================================================
#  Host Tools & Resources
# ============================================================================

$(IMG2RAW): tools/img2raw.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -O2 -g -Itools $< -lm -o $@

$(LOGO_BIN): assets/logo.png $(IMG2RAW)
	@mkdir -p $(dir $@)
	$(IMG2RAW) $< $@ 256

$(WALLPAPER_BIN): assets/owl_wallpaper.jpg $(IMG2RAW)
	@mkdir -p $(dir $@)
	$(IMG2RAW) $< $@ 1920

$(AUTHD_ELF): src/os/apps/authd.c src/os/apps/user.ld $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -nostdlib -Wl,-T,src/os/apps/user.ld -o $@ src/os/apps/authd.c $(LIBC_OBJS)
	@echo "[SVC_AUTH] $@"

$(HELLO_ELF): src/os/apps/hello.c src/os/apps/user.ld $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -nostdlib -Wl,-T,src/os/apps/user.ld -o $@ src/os/apps/hello.c $(LIBC_OBJS)
	@echo "[APP]  $@"

$(EXEC_TEST_ELF): src/os/apps/exec_test.c src/os/apps/user.ld $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -nostdlib -Wl,-T,src/os/apps/user.ld -o $@ src/os/apps/exec_test.c $(LIBC_OBJS)
	@echo "[APP]  $@"

$(ORPHAN_TEST_ELF): src/os/apps/orphan_test.c src/os/apps/user.ld $(LIBC_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -nostdlib -Wl,-T,src/os/apps/user.ld -o $@ src/os/apps/orphan_test.c $(LIBC_OBJS)
	@echo "[APP]  $@"

$(DISK_IMG):
	@mkdir -p $(dir $@)
	truncate -s 32M $@
	@if command -v mkfs.fat >/dev/null 2>&1; then \
		mkfs.fat -F 32 -n YAMOS $@ >/dev/null; \
	elif command -v mformat >/dev/null 2>&1; then \
		mformat -i $@ -F -v YAMOS ::; \
	else \
		echo "[DISK] warning: mkfs.fat/mformat not found; disk will remain unformatted"; \
	fi
	@echo "[DISK] $@"

# ============================================================================
#  ISO Creation (Limine-based bootable ISO)
# ============================================================================

$(KERNEL_ISO): $(KERNEL_ELF) $(LOGO_BIN) $(WALLPAPER_BIN) $(AUTHD_ELF) $(HELLO_ELF) $(EXEC_TEST_ELF) $(ORPHAN_TEST_ELF)
	@echo "[ISO]  Building bootable ISO..."
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)/boot/limine
	@mkdir -p $(ISO_DIR)/boot/grub
	@mkdir -p $(ISO_DIR)/EFI/BOOT
	
	# Copy kernel and modules
	cp $(KERNEL_ELF) $(ISO_DIR)/boot/$(KERNEL_NAME).elf
	cp $(LOGO_BIN) $(ISO_DIR)/boot/logo.bin
	cp $(WALLPAPER_BIN) $(ISO_DIR)/boot/wallpaper.bin
	cp $(AUTHD_ELF) $(ISO_DIR)/boot/authd.elf
	cp $(HELLO_ELF) $(ISO_DIR)/boot/hello.elf
	cp $(EXEC_TEST_ELF) $(ISO_DIR)/boot/exec-test.elf
	cp $(ORPHAN_TEST_ELF) $(ISO_DIR)/boot/orphan-test.elf
	
	# Copy limine config
	cp limine.conf $(ISO_DIR)/boot/limine/limine.conf
	
	# Copy limine binaries (built via make setup)
	cp limine-git/limine-bios.sys $(ISO_DIR)/boot/limine/ 2>/dev/null || true
	cp limine-git/limine-bios-cd.bin $(ISO_DIR)/boot/limine/ 2>/dev/null || true
	cp limine-git/limine-uefi-cd.bin $(ISO_DIR)/boot/limine/ 2>/dev/null || true
	cp limine-git/BOOTX64.EFI $(ISO_DIR)/EFI/BOOT/ 2>/dev/null || true
	cp limine-git/BOOTIA32.EFI $(ISO_DIR)/EFI/BOOT/ 2>/dev/null || true
	
	# Create ISO with xorriso
	xorriso -as mkisofs \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_DIR) -o $@
	
	# Install limine to ISO
	./limine-git/limine bios-install $@ 2>/dev/null || true
	
	@echo "============================================"
	@echo "  YamKernel ISO ready: $@"
	@echo "  Use with VMware, VirtualBox, or bare metal"
	@echo "============================================"

# ============================================================================
#  Run in QEMU
# ============================================================================

run: $(KERNEL_ISO) $(DISK_IMG)
	qemu-system-x86_64 \
		-cdrom $(KERNEL_ISO) \
		-drive file=$(DISK_IMG),format=raw,if=none,id=vd0 \
		-device virtio-blk-pci,drive=vd0,disable-modern=on \
		-serial stdio \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-m 256M \
		-smp 2 \
		-boot d \
		-no-reboot \
		-no-shutdown

# Run with GDB server enabled (listens on localhost:1234)
debug: $(KERNEL_ISO) $(DISK_IMG)
	qemu-system-x86_64 \
		-cdrom $(KERNEL_ISO) \
		-drive file=$(DISK_IMG),format=raw,if=none,id=vd0 \
		-device virtio-blk-pci,drive=vd0,disable-modern=on \
		-serial stdio \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-m 256M \
		-smp 2 \
		-boot d \
		-s -S \
		-no-reboot \
		-no-shutdown

# Run with UEFI (requires OVMF)
run-uefi: $(KERNEL_ISO) $(DISK_IMG)
	qemu-system-x86_64 \
		-cdrom $(KERNEL_ISO) \
		-drive file=$(DISK_IMG),format=raw,if=none,id=vd0 \
		-device virtio-blk-pci,drive=vd0,disable-modern=on \
		-bios /usr/share/OVMF/OVMF_CODE.fd \
		-serial stdio \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-m 256M \
		-smp 2 \
		-boot d \
		-no-reboot \
		-no-shutdown

# Run with serial output captured to a log file
run-serial: $(KERNEL_ISO) $(DISK_IMG)
	@echo "[SERIAL] Starting QEMU with serial log -> build/serial.log"
	@echo "[SERIAL] View log:  tail -f build/serial.log"
	qemu-system-x86_64 \
		-cdrom $(KERNEL_ISO) \
		-drive file=$(DISK_IMG),format=raw,if=none,id=vd0 \
		-device virtio-blk-pci,drive=vd0,disable-modern=on \
		-serial file:build/serial.log \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-m 256M \
		-smp 2 \
		-boot d \
		-no-reboot \
		-no-shutdown

# Run headless — serial only, no graphical window (fastest for CI/debug)
run-serial-only: $(KERNEL_ISO) $(DISK_IMG)
	@echo "[SERIAL] Headless mode — all output on terminal"
	qemu-system-x86_64 \
		-cdrom $(KERNEL_ISO) \
		-drive file=$(DISK_IMG),format=raw,if=none,id=vd0 \
		-device virtio-blk-pci,drive=vd0,disable-modern=on \
		-nographic \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-m 256M \
		-smp 2 \
		-boot d \
		-no-reboot \
		-no-shutdown

# Bounded serial-log verification for development/CI. Prefer this over ad-hoc
# QEMU commands when checking boot regressions.
verify-log: $(KERNEL_ISO) $(DISK_IMG)
	@echo "[VERIFY] Running bounded QEMU boot -> build/verify.log"
	@rm -f build/verify.log
	@timeout 120s qemu-system-x86_64 \
		-cdrom $(KERNEL_ISO) \
		-drive file=$(DISK_IMG),format=raw,if=none,id=vd0 \
		-device virtio-blk-pci,drive=vd0,disable-modern=on \
		-serial file:build/verify.log \
		-display none \
		-netdev user,id=net0 \
		-device e1000,netdev=net0 \
		-m 256M \
		-smp 2 \
		-boot d \
		-no-reboot \
		-no-shutdown >/dev/null 2>&1 || true
	@echo "[VERIFY] Key boot evidence:"
	@grep -E "\[INIT\]|\[ELF\]|\[EXEC_TEST\]|\[HELLO_EXEC\]|\[ORPHAN_TEST\]|\[WAYLAND\]|\[WL_DBG\]|\[EXC\]|\[PCI\]|\[DRIVER\]|\[BLOCK\]|\[VBLK\]|\[VFS\]|\[FAT32\]|\[e1000\]|\[DHCP\]|\[DNS\]|\[TCP\]|\[HTTP\]|PANIC|EXCEPTION|FAULT" build/verify.log | tail -n 260 || true

# ============================================================================
#  Setup (install dependencies on Debian/Ubuntu/WSL)
# ============================================================================

setup:
	@echo "[SETUP] Installing build dependencies..."
	sudo apt update
	sudo apt install -y nasm gcc make xorriso mtools qemu-system-x86 ovmf git
	@if [ ! -d "limine-git" ]; then \
		echo "[SETUP] Cloning and building Limine bootloader..."; \
		git clone https://github.com/limine-bootloader/limine.git limine-git --branch=v8.x-binary --depth=1; \
		make -C limine-git; \
	fi
	@echo "[SETUP] Done! Run 'make iso' to build."

clean:
	rm -rf $(BUILD_DIR)
	@echo "[CLEAN] Done."
