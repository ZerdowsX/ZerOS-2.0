CC = gcc
AS = nasm
LD = ld

CFLAGS = -m32 -ffreestanding -fno-pic -fno-stack-protector -nostdlib -Wall -Wextra -std=gnu99
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld

BUILD_DIR = build
ISO_DIR = $(BUILD_DIR)/isodir
GRUB_MKRESCUE ?= $(shell command -v grub-mkrescue 2>/dev/null || command -v grub2-mkrescue 2>/dev/null)

.PHONY: all iso run run-iso clean

all: iso

boot.o: boot.asm
	@echo "[AS] boot.asm -> boot.o"
	@$(AS) $(ASFLAGS) boot.asm -o boot.o

kernel.o: kernel.c
	@echo "[CC] kernel.c -> kernel.o"
	@$(CC) $(CFLAGS) -c kernel.c -o kernel.o

zerdos.bin: boot.o kernel.o linker.ld
	@echo "[LD] boot.o + kernel.o -> zerdos.bin"
	@$(LD) $(LDFLAGS) -z noexecstack boot.o kernel.o -o zerdos.bin

iso: zerdos.bin
	@if [ -z "$(GRUB_MKRESCUE)" ]; then \
		echo "Error: grub-mkrescue (or grub2-mkrescue) is not installed."; \
		echo "Install grub tools and xorriso, then run 'make iso' again."; \
		exit 1; \
	fi
	@echo "[ISO] Building bootable zerdos.iso"
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp zerdos.bin $(ISO_DIR)/boot/zerdos.bin
	@cp iso/boot/grub/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg
	@$(GRUB_MKRESCUE) -o zerdos.iso $(ISO_DIR)

run: zerdos.bin
	@echo "[RUN] qemu-system-i386 -kernel zerdos.bin"
	@qemu-system-i386 -kernel zerdos.bin

run-iso: iso
	@echo "[RUN] qemu-system-i386 -cdrom zerdos.iso"
	@qemu-system-i386 -cdrom zerdos.iso

clean:
	@echo "[CLEAN] Removing build artifacts"
	@rm -rf *.o *.bin *.iso $(BUILD_DIR)
