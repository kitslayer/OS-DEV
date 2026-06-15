# OS-DEV — build a from-scratch x86_64 kernel and run it under QEMU.
#
#   make          build build/kernel.elf
#   make run      boot it in QEMU with a graphical window
#   make test     boot it headless and confirm via serial output (no window)
#   make clean    remove build artifacts

# --- tools ------------------------------------------------------------------
CC      := gcc
AS      := nasm
LD      := ld
OBJCOPY := objcopy
QEMU    := qemu-system-x86_64

# --- flags ------------------------------------------------------------------
# Freestanding kernel C: no host runtime, no PIC, no red zone (interrupts would
# clobber it), and no SSE/MMX/x87 (we haven't enabled the FPU yet).
CFLAGS  := -std=gnu11 -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mgeneral-regs-only -fwrapv \
           -Wall -Wextra -Ikernel/include -O2 -g

ASFLAGS := -f elf64
LDFLAGS := -n -T linker.ld

QEMUFLAGS := -no-reboot -no-shutdown

# --- sources ----------------------------------------------------------------
BUILD   := build
# kernel.elf: real ELF64 with symbols (use this for gdb).
# kernel32.elf: same code repackaged in a 32-bit ELF container so QEMU's
#               multiboot loader will accept it (it refuses ELF64).
KERNEL64 := $(BUILD)/kernel.elf
KERNEL   := $(BUILD)/kernel32.elf
DISK      := $(BUILD)/fat.img
DISKFLAGS := -drive file=$(BUILD)/fat.img,format=raw,if=ide
# An e1000 NIC on user-mode (SLIRP) networking: the gateway 10.0.2.2 answers
# ARP and ICMP, which is how we test the network stack.
NICFLAGS  := -netdev user,id=net0 -device e1000,netdev=net0
# A UHCI USB controller + an absolute pointing device (tablet).
USBFLAGS  := -device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0

C_SRCS  := $(shell find kernel -name '*.c')
ASM_SRCS:= $(shell find boot kernel -name '*.asm')
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS)) \
           $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS))

# --- rules ------------------------------------------------------------------
.PHONY: all run test jstest clean

all: $(KERNEL) $(DISK)

# --- FAT32 disk image (built by our host-side tool) --------------------------
$(BUILD)/mkfatfs: tools/mkfatfs.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -o $@ $<

$(DISK): $(BUILD)/mkfatfs
	$(BUILD)/mkfatfs $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# -fwrapv is in CFLAGS above (global): the kernel parses untrusted input (network,
# disk, images, TLS, HTML, JS) with signed arithmetic, so signed overflow must be
# defined (wrapping) rather than UB under -O2 across the whole kernel.

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# --- userspace ---------------------------------------------------------------
# Each user program (shell, clock, ...) is linked with the shared ulib into its
# own ELF; the kernel embeds them all (see kernel/asm/user_blob.asm).
USER_CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone \
               -mgeneral-regs-only -std=gnu11 -O2 -Wall -Ikernel/include
USER_ELFS := $(BUILD)/shell.elf $(BUILD)/clock.elf $(BUILD)/calc.elf $(BUILD)/snake.elf $(BUILD)/editor.elf $(BUILD)/g2048.elf $(BUILD)/life.elf $(BUILD)/tetris.elf $(BUILD)/breakout.elf $(BUILD)/mines.elf $(BUILD)/sudoku.elf

$(BUILD)/user_%.o: user/%.c
	@mkdir -p $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/%.elf: $(BUILD)/user_%.o $(BUILD)/user_ulib.o user/user.ld
	$(LD) -T user/user.ld -o $@ $(BUILD)/user_$*.o $(BUILD)/user_ulib.o
	@echo "Built $@ (userspace program)"

# the embedded blob depends on every program ELF
$(BUILD)/kernel/asm/user_blob.o: $(USER_ELFS)

$(KERNEL64): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(KERNEL): $(KERNEL64)
	$(OBJCOPY) -I elf64-x86-64 -O elf32-i386 $< $@
	@echo "Built $@ (64-bit kernel in a multiboot-loadable 32-bit container)"

# Interactive: opens a QEMU window so you can see the VGA output.
run: $(KERNEL) $(DISK)
	$(QEMU) $(QEMUFLAGS) -kernel $(KERNEL) $(DISKFLAGS) $(NICFLAGS) $(USBFLAGS) -serial stdio

# Headless smoke test: no window, capture serial, kill after a few seconds.
# Used to confirm the kernel boots without needing a display.
test: $(KERNEL) $(DISK)
	@echo "--- booting headless, capturing COM1 (5s) ---"
	@timeout 5 $(QEMU) $(QEMUFLAGS) -kernel $(KERNEL) $(DISKFLAGS) $(NICFLAGS) $(USBFLAGS) \
	    -display none -serial stdio ; \
	    echo "--- qemu exited ---"

# Host-side regression test of the from-scratch JavaScript engine (ASan+UBSan).
jstest:
	@tests/run-js-tests.sh

clean:
	rm -rf $(BUILD)
