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
# An Intel AC'97 audio codec. Override the host backend if needed:
#   make run AUDIODEV=sdl   (or alsa, pa, none)
AUDIODEV  ?= pa
AUDIOFLAGS := -device AC97,audiodev=snd0 -audiodev $(AUDIODEV),id=snd0
# Same AC'97 codec, but with the null backend so the headless smoke test can
# exercise the audio bring-up without depending on a host sound server.
TESTAUDIOFLAGS := -device AC97,audiodev=snd0 -audiodev none,id=snd0

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
USER_ELFS := $(BUILD)/shell.elf $(BUILD)/clock.elf $(BUILD)/calc.elf $(BUILD)/snake.elf $(BUILD)/editor.elf $(BUILD)/g2048.elf $(BUILD)/life.elf $(BUILD)/tetris.elf $(BUILD)/breakout.elf $(BUILD)/mines.elf $(BUILD)/sudoku.elf $(BUILD)/calendar.elf $(BUILD)/mandel.elf $(BUILD)/piano.elf $(BUILD)/maze.elf $(BUILD)/adv.elf $(BUILD)/matrix.elf $(BUILD)/paint.elf $(BUILD)/hangman.elf $(BUILD)/jukebox.elf $(BUILD)/ttt.elf $(BUILD)/bj.elf $(BUILD)/typing.elf $(BUILD)/simon.elf $(BUILD)/c4.elf $(BUILD)/wordle.elf $(BUILD)/gfxdemo.elf $(BUILD)/doom.elf $(BUILD)/quake.elf

$(BUILD)/user_%.o: user/%.c
	@mkdir -p $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/%.elf: $(BUILD)/user_%.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld
	$(LD) -T user/user.ld -o $@ $(BUILD)/user_$*.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (userspace program)"

# --- DOOM (vendored doomgeneric) ---------------------------------------------
# Same shape as USER_CFLAGS but WITH floating point (DOOM uses double): drop
# -mgeneral-regs-only, add SSE. -w silences DOOM's many legacy warnings;
# -fcommon tolerates its tentative-definition globals. The shim headers in
# user/doom/include stand in for libc.
DOOM_CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone \
               -std=gnu11 -O2 -w -fcommon -msse2 -mfpmath=sse \
               -Iuser/doom/include -Ikernel/include \
               -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DNORMALUNIX

DOOM_SRCS := $(wildcard user/doom/*.c)
DOOM_OBJS := $(patsubst user/doom/%.c,$(BUILD)/doom/%.o,$(DOOM_SRCS))

$(BUILD)/doom/%.o: user/doom/%.c Makefile
	@mkdir -p $(BUILD)/doom
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

$(BUILD)/doom.elf: $(DOOM_OBJS) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld
	$(LD) -T user/user.ld -o $@ $(DOOM_OBJS) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (DOOM)"

# --- Quake (vendored quakegeneric) -------------------------------------------
# Same shape as DOOM_CFLAGS: floating point on (Quake is FP-heavy) so drop
# -mgeneral-regs-only and add SSE; -w silences the legacy warnings; -fcommon
# tolerates the engine's tentative-definition globals. The shim headers in
# user/quake/include stand in for libc; -Iuser/quake lets the engine .c files
# resolve their own cross-included headers. The engine renders at vid_null.c's
# 320x240, which is what quakegeneric.h's RES_X/RES_Y default to.
QUAKE_CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone \
                -std=gnu11 -O2 -w -fcommon -msse2 -mfpmath=sse \
                -Iuser/quake/include -Iuser/quake -Ikernel/include \
                -DQUAKEGENERIC_RESX=320 -DQUAKEGENERIC_RESY=240 -DNORMALUNIX

QUAKE_SRCS := $(wildcard user/quake/*.c)
QUAKE_OBJS := $(patsubst user/quake/%.c,$(BUILD)/quake/%.o,$(QUAKE_SRCS))

$(BUILD)/quake/%.o: user/quake/%.c Makefile
	@mkdir -p $(BUILD)/quake
	$(CC) $(QUAKE_CFLAGS) -c $< -o $@

$(BUILD)/quake.elf: $(QUAKE_OBJS) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld
	$(LD) -T user/user.ld -o $@ $(QUAKE_OBJS) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (Quake)"

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
	$(QEMU) $(QEMUFLAGS) -kernel $(KERNEL) $(DISKFLAGS) $(NICFLAGS) $(USBFLAGS) $(AUDIOFLAGS) -serial stdio

# Headless smoke test: no window, capture serial, kill after a few seconds.
# Used to confirm the kernel boots without needing a display.
test: $(KERNEL) $(DISK)
	@echo "--- booting headless, capturing COM1 (5s) ---"
	@timeout 5 $(QEMU) $(QEMUFLAGS) -kernel $(KERNEL) $(DISKFLAGS) $(NICFLAGS) $(USBFLAGS) $(TESTAUDIOFLAGS) \
	    -display none -serial stdio ; \
	    echo "--- qemu exited ---"

# Host-side regression test of the from-scratch JavaScript engine (ASan+UBSan).
jstest:
	@tests/run-js-tests.sh

# Host-side regression + fuzz test of the image decoders (ASan+UBSan).
imgtest:
	@tests/run-img-tests.sh

# Host-side fuzz test of the X.509 certificate parser (ASan+UBSan).
x509test:
	@tests/run-x509-tests.sh

# Host-side fuzz test of the TCP/IP packet parser + reassembly (ASan+UBSan).
nettest:
	@tests/run-net-tests.sh

# Host-side fuzz test of the FAT32 read path over corrupt/cyclic on-disk structures (ASan+UBSan).
fstest:
	@tests/run-fs-tests.sh

# Host-side known-answer test of the crypto primitives vs published RFC/FIPS vectors (ASan+UBSan).
kattest:
	@tests/run-crypto-tests.sh

# Host-side fuzz test of the SVG rasterizer over adversarial/truncated XML (ASan+UBSan).
svgtest:
	@tests/run-svg-tests.sh

# Host-side round-trip test of the DEFLATE/gzip compressor vs the decoder (ASan+UBSan).
deflatetest:
	@tests/run-deflate-tests.sh

# Host-side round-trip test of the PNG encoder vs the decoder (ASan+UBSan).
pngenctest:
	@tests/run-pngenc-tests.sh

# Host-side extraction + corrupt-input fuzz of the ZIP extractor (ASan+UBSan).
ziptest:
	@tests/run-zip-tests.sh

# Host-side extraction + corrupt-input fuzz of the tar (ustar) extractor (ASan+UBSan).
tartest:
	@tests/run-tar-tests.sh

# Host-side regression test of the userspace malloc/free allocator (ASan+UBSan).
heaptest:
	@tests/run-heap-tests.sh

# Host-side regression + fuzz test of the WAV header parser (ASan+UBSan).
wavtest:
	@tests/run-wav-tests.sh

# Host-side regression + fuzz test of the ELF64 loader (ASan+UBSan): the ring-3
# trust boundary — a malformed program must never OOB-read or escape its range.
elftest:
	@tests/run-elf-tests.sh

# Host-side regression + fuzz test of the HTTP/1.x response parsers (ASan+UBSan):
# chunked-transfer decode + header scans over untrusted/truncated server bytes.
httptest:
	@tests/run-http-tests.sh

# Host-side torture + invariant test of the KERNEL heap kmalloc/kfree (ASan+UBSan).
kheaptest:
	@tests/run-kheap-tests.sh

# Host-side fuzz of the engine's JSON.parse over untrusted/malformed/deep input (ASan+UBSan).
jsonfuzztest:
	@tests/run-jsonfuzz-tests.sh

# Host-side fuzz of the engine's regex (compile + backtracking search) over ReDoS/malformed input (ASan+UBSan).
regexfuzztest:
	@tests/run-regexfuzz-tests.sh

# Host-side fuzz of the full JS parse+run pipeline on untrusted/malformed source (ASan+UBSan).
jssrcfuzztest:
	@tests/run-jssrcfuzz-tests.sh

# Host-side fuzz of the HTML entity decoder over untrusted/malformed page bytes (ASan+UBSan).
htmlentfuzztest:
	@tests/run-htmlentfuzz-tests.sh

# In-guest boot assertion: boots the real kernel headless and asserts every
# bring-up marker is present with no crash (exercises the whole driver stack,
# not one .c in isolation). SKIPs cleanly if QEMU is absent.
boottest: $(KERNEL) $(DISK)
	@tests/run-boot-tests.sh

# In-guest GRAPHICAL assertion: boots, lets the desktop paint, captures the VGA
# framebuffer via the QEMU monitor and asserts it rendered (compositor / fb /
# font / vga.c -- no other in-guest coverage). SKIPs if QEMU/socat/python3 absent.
gfxtest: $(KERNEL) $(DISK)
	@tests/run-gfx-tests.sh

# Run every host-side regression/fuzz/KAT suite, then the in-guest boot assertions.
# ('test' above is the human-readable headless boot; 'boottest'/'gfxtest' are asserted.)
check: jstest imgtest x509test nettest fstest kattest svgtest deflatetest pngenctest ziptest tartest heaptest wavtest elftest httptest kheaptest jsonfuzztest regexfuzztest jssrcfuzztest htmlentfuzztest boottest gfxtest
	@echo "ALL TESTS PASSED (jstest + imgtest + x509test + nettest + fstest + kattest + svgtest + deflatetest + pngenctest + ziptest + tartest + heaptest + wavtest + elftest + httptest + kheaptest + jsonfuzztest + regexfuzztest + jssrcfuzztest + htmlentfuzztest + boottest + gfxtest)"

clean:
	rm -rf $(BUILD)
