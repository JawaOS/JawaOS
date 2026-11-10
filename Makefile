ARCH ?= x86_64

LIMINE_VER  := 12.9.6
LIMINE_URL  := https://github.com/limine-bootloader/limine/releases/download/v$(LIMINE_VER)/limine-binary.tar.xz
LIMINE_DIR  := third_party/limine

BUILD  := build/$(ARCH)
KERNEL := $(BUILD)/jawaos.elf
ISO    := $(BUILD)/jawaos-$(ARCH).iso

CC := clang
LD := ld.lld

COMMON_CFLAGS := -std=gnu11 -Wall -Wextra -O2 -g \
	         -ffreestanding -fno-stack-protector -fno-stack-check \
	         -fno-lto -fno-PIC -Ikernel

ifeq ($(ARCH),x86_64)
  TARGET_CFLAGS := --target=x86_64-unknown-none-elf -m64 -march=x86-64 \
	           -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	           -mno-red-zone -mcmodel=kernel
else ifeq ($(ARCH),aarch64)
  TARGET_CFLAGS := --target=aarch64-unknown-none-elf -mgeneral-regs-only
else ifeq ($(ARCH),riscv64)
  TARGET_CFLAGS := --target=riscv64-unknown-none-elf \
	           -march=rv64imac_zicsr_zifencei -mabi=lp64 \
	           -mno-relax -mcmodel=medany
else
  $(error Unsupported ARCH '$(ARCH)' (use x86_64, aarch64 or riscv64))
endif

CFLAGS  := $(COMMON_CFLAGS) $(TARGET_CFLAGS)
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 \
	   -T kernel/linker-$(ARCH).ld

ifdef DEBUG
  CFLAGS += -DJAWA_DEBUG
endif

OBJ := $(BUILD)/main.o $(BUILD)/console.o $(BUILD)/string.o $(BUILD)/serial.o \
       $(BUILD)/fbcon.o $(BUILD)/kprintf.o $(BUILD)/panic.o \
       $(BUILD)/boot.o $(BUILD)/spinlock.o $(BUILD)/pmm.o \
       $(BUILD)/vmm.o $(BUILD)/kheap.o \
       $(BUILD)/int.o \
       $(BUILD)/timer.o $(BUILD)/thread.o

.PHONY: all kernel iso run run-uefi limine clean distclean

all: kernel

kernel: $(KERNEL)

$(BUILD)/%.o: kernel/%.c $(wildcard kernel/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJ) kernel/linker-$(ARCH).ld
	$(LD) $(LDFLAGS) $(OBJ) -o $@

limine: $(LIMINE_DIR)/limine

$(LIMINE_DIR)/limine:
	@if [ ! -d $(LIMINE_DIR) ]; then \
	    mkdir -p third_party; \
	    curl -fsSL $(LIMINE_URL) -o third_party/limine-binary.tar.xz; \
	    tar -xJf third_party/limine-binary.tar.xz -C third_party; \
	    mv third_party/limine-binary $(LIMINE_DIR); \
	fi
	$(MAKE) -C $(LIMINE_DIR)

iso: $(ISO)

$(ISO): $(KERNEL) boot/limine.conf $(LIMINE_DIR)/limine
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/jawaos.elf
	cp boot/limine.conf $(BUILD)/iso_root/boot/limine/limine.conf
ifeq ($(ARCH),x86_64)
	cp $(LIMINE_DIR)/limine-bios.sys $(LIMINE_DIR)/limine-bios-cd.bin \
	   $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI $(LIMINE_DIR)/BOOTIA32.EFI \
	   $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
	    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
	    -boot-info-table -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(BUILD)/iso_root -o $@
	$(LIMINE_DIR)/limine bios-install $@
else ifeq ($(ARCH),aarch64)
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTAA64.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
	    -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(BUILD)/iso_root -o $@
else ifeq ($(ARCH),riscv64)
	cp $(LIMINE_DIR)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTRISCV64.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -R -r -J \
	    -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(BUILD)/iso_root -o $@
endif

OVMF_X64     ?= /usr/share/OVMF/OVMF_CODE_4M.fd
AAVMF        ?= /usr/share/AAVMF/AAVMF_CODE.fd
RISCV_FW     ?= /usr/share/qemu-efi-riscv64/RISCV_VIRT_CODE.fd

run: $(ISO)
ifeq ($(ARCH),x86_64)
	qemu-system-x86_64 -M q35 -m 256M -cdrom $(ISO) -boot d -serial stdio
else ifeq ($(ARCH),aarch64)
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 256M \
	    -drive if=pflash,format=raw,readonly=on,file=$(AAVMF) \
	    -device ramfb -device qemu-xhci -device usb-kbd \
	    -cdrom $(ISO) -serial stdio
else ifeq ($(ARCH),riscv64)
	qemu-system-riscv64 -M virt -cpu rv64 -m 256M \
	    -drive if=pflash,format=raw,unit=0,readonly=on,file=$(RISCV_FW) \
	    -device ramfb -device qemu-xhci -device usb-kbd \
	    -drive file=$(ISO),if=none,format=raw,id=iso0 \
	    -device virtio-blk-pci,drive=iso0 -serial stdio
endif

run-uefi: $(ISO)
	qemu-system-x86_64 -M q35 -m 256M \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_X64) \
	    -cdrom $(ISO) -boot d

clean:
	rm -rf build

distclean: clean
	rm -rf third_party
