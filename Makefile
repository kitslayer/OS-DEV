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
           -Wall -Wextra -Ikernel/include -O2 -g -MMD -MP

ASFLAGS := -f elf64
LDFLAGS := -n -T linker.ld

# -m 256M: QEMU's ~128M default starves the heaviest app — Quake needs its 18 MB
# PAK + a multi-MB hunk on top of the kernel (incl. the 40 MB JS arena), so it
# silently fails to launch at 128M but runs fine at 256M (DOOM, lighter, works at
# either). 256M comfortably fits the whole app suite, even DOOM+Quake at once.
QEMUFLAGS := -no-reboot -no-shutdown -m 256M

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
# Same SLIRP network, but presenting a Realtek RTL8139 instead of the e1000 — to
# exercise the second NIC driver (kernel/rtl8139.c). Used by `make run-rtl8139`.
RTLNICFLAGS := -netdev user,id=net0 -device rtl8139,netdev=net0
# Same SLIRP network, but presenting a LEGACY virtio-net NIC instead of the e1000
# — to exercise the paravirtual NIC driver (kernel/virtio_net.c). disable-modern
# forces the legacy I/O-port transport the driver speaks. Used by `make run-virtio-net`.
VIRTIONICFLAGS := -netdev user,id=net0 -device virtio-net-pci,netdev=net0,disable-modern=on,disable-legacy=off
# A UHCI USB controller + an absolute pointing device (tablet).
USBFLAGS  := -device piix3-usb-uhci,id=uhci -device usb-tablet,bus=uhci.0
# An Intel AC'97 audio codec. Override the host backend if needed:
#   make run AUDIODEV=sdl   (or alsa, pa, none)
AUDIODEV  ?= pa
AUDIOFLAGS := -device AC97,audiodev=snd0 -audiodev $(AUDIODEV),id=snd0
# Same AC'97 codec, but with the null backend so the headless smoke test can
# exercise the audio bring-up without depending on a host sound server.
TESTAUDIOFLAGS := -device AC97,audiodev=snd0 -audiodev none,id=snd0
# An Intel HD Audio controller (`intel-hda`) with an output codec (`hda-output`)
# instead of AC'97 — drives kernel/hda.c. The audiodev attaches to the codec
# (hda-output), not the controller bus. Override the host backend like AUDIODEV.
HDAAUDIOFLAGS := -device intel-hda -device hda-output,audiodev=snd0 -audiodev $(AUDIODEV),id=snd0
# Same HDA pair on the null backend, for the headless hdatest (no host sound server).
TESTHDAFLAGS := -device intel-hda -device hda-output,audiodev=snd0 -audiodev none,id=snd0

C_SRCS  := $(shell find kernel -name '*.c')
ASM_SRCS:= $(shell find boot kernel -name '*.asm')
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS)) \
           $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS))

# --- rules ------------------------------------------------------------------
.PHONY: all run run-rtl8139 run-virtio-net run-hda test rtl8139test virtionettest virtioblktest nvmetest parttest virtiogputest hdatest jstest clean

all: $(KERNEL) $(DISK)

# --- FAT32 disk image (built by our host-side tool) --------------------------
$(BUILD)/mkfatfs: tools/mkfatfs.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -o $@ $<

$(DISK): $(BUILD)/mkfatfs
	$(BUILD)/mkfatfs $@

$(BUILD)/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# js.o uses real IEEE-754 doubles (M906), which need SSE — so drop -mgeneral-regs-only
# and add SSE for this one translation unit (an exact-target rule overrides the %.o rule).
# Safe in the kernel: fpu_init() enables x87+SSE at boot, and the scheduler saves/restores
# FP/SSE state for EVERY task (task.c fx_alloc in sched_init + task_create_stack), so the
# JS engine's xmm use survives context switches. No libm/libcall (js_sqrt/js_pow are local).
$(BUILD)/kernel/js.o: kernel/js.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse2 -mfpmath=sse -c $< -o $@

# -fwrapv is in CFLAGS above (global): the kernel parses untrusted input (network,
# disk, images, TLS, HTML, JS) with signed arithmetic, so signed overflow must be
# defined (wrapping) rather than UB under -O2 across the whole kernel.

$(BUILD)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

# --- userspace ---------------------------------------------------------------
# Each user program (shell, clock, ...) is linked with the shared ulib into its
# own ELF; the kernel embeds them all (see kernel/asm/user_blob.asm).
# -fwrapv: the OS-authored apps (shell $((...)) / calc evaluators, etc.) do signed
# arithmetic on user input, so overflow must wrap (defined) rather than be UB under
# -O2 — same rationale as the kernel CFLAGS. (Ported games keep their own CFLAGS.)
USER_CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone \
               -mgeneral-regs-only -std=gnu11 -O2 -fwrapv -Wall -Ikernel/include -MMD -MP
USER_ELFS := $(BUILD)/shell.elf $(BUILD)/clock.elf $(BUILD)/calc.elf $(BUILD)/snake.elf $(BUILD)/editor.elf $(BUILD)/g2048.elf $(BUILD)/life.elf $(BUILD)/tetris.elf $(BUILD)/breakout.elf $(BUILD)/mines.elf $(BUILD)/sudoku.elf $(BUILD)/calendar.elf $(BUILD)/timer.elf $(BUILD)/mandel.elf $(BUILD)/piano.elf $(BUILD)/maze.elf $(BUILD)/adv.elf $(BUILD)/matrix.elf $(BUILD)/paint.elf $(BUILD)/hangman.elf $(BUILD)/jukebox.elf $(BUILD)/ttt.elf $(BUILD)/bj.elf $(BUILD)/typing.elf $(BUILD)/simon.elf $(BUILD)/c4.elf $(BUILD)/wordle.elf $(BUILD)/gfxdemo.elf $(BUILD)/doom.elf $(BUILD)/quake.elf $(BUILD)/nes.elf $(BUILD)/reversi.elf $(BUILD)/lights.elf $(BUILD)/fifteen.elf $(BUILD)/mastermind.elf $(BUILD)/pong.elf $(BUILD)/halflife.elf $(BUILD)/memory.elf $(BUILD)/sokoban.elf $(BUILD)/battleship.elf $(BUILD)/pig.elf $(BUILD)/raycast.elf $(BUILD)/tron.elf $(BUILD)/spaceinv.elf $(BUILD)/asteroids.elf $(BUILD)/flappy.elf $(BUILD)/gb.elf $(BUILD)/lander.elf $(BUILD)/yahtzee.elf $(BUILD)/checkers.elf $(BUILD)/gomoku.elf $(BUILD)/frogger.elf $(BUILD)/chess.elf $(BUILD)/vpoker.elf $(BUILD)/mancala.elf $(BUILD)/dotsbox.elf $(BUILD)/missile.elf $(BUILD)/pacman.elf $(BUILD)/solitaire.elf $(BUILD)/gems.elf $(BUILD)/columns.elf $(BUILD)/freecell.elf $(BUILD)/spider.elf

$(BUILD)/user_%.o: user/%.c Makefile
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

# --- NES (vendored libxnes + a platform shim) --------------------------------
# Same shape as DOOM_CFLAGS: floating point on (libxnes's audio sample path is
# float) so drop -mgeneral-regs-only and add SSE; -w silences the vendored
# core's legacy warnings; -fcommon tolerates its tentative-definition globals.
# -Iuser/nes lets the core's <xnes.h>/<cpu.h>/... resolve; -Iuser/nes/include
# supplies the libc shim headers (GCC provides <stdint.h>/<stdarg.h>/<limits.h>
# freestanding). The emulator renders a 256x240 XRGB framebuffer.
NES_CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone \
              -std=gnu11 -O2 -w -fcommon -msse2 -mfpmath=sse \
              -Iuser/nes -Iuser/nes/include -Ikernel/include

NES_SRCS := $(wildcard user/nes/*.c)
NES_OBJS := $(patsubst user/nes/%.c,$(BUILD)/nes/%.o,$(NES_SRCS))

$(BUILD)/nes/%.o: user/nes/%.c Makefile
	@mkdir -p $(BUILD)/nes
	$(CC) $(NES_CFLAGS) -c $< -o $@

$(BUILD)/nes.elf: $(NES_OBJS) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld
	$(LD) -T user/user.ld -o $@ $(NES_OBJS) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (NES)"

# --- Game Boy (vendored Peanut-GB, single header + a platform shim) ----------
# Same shape as the NES stanza; -Iuser/gb resolves <peanut_gb.h>, -Iuser/gb/include
# the libc shim headers. SSE on (harmless) for consistency with the other ports.
GB_CFLAGS := -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone \
             -std=gnu11 -O2 -w -fcommon -msse2 -mfpmath=sse \
             -Iuser/gb -Iuser/gb/include -Ikernel/include

GB_SRCS := $(wildcard user/gb/*.c)
GB_OBJS := $(patsubst user/gb/%.c,$(BUILD)/gb/%.o,$(GB_SRCS))

$(BUILD)/gb/%.o: user/gb/%.c Makefile
	@mkdir -p $(BUILD)/gb
	$(CC) $(GB_CFLAGS) -c $< -o $@

$(BUILD)/gb.elf: $(GB_OBJS) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld
	$(LD) -T user/user.ld -o $@ $(GB_OBJS) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (Game Boy)"

# --- Raycaster (a from-scratch pseudo-3D maze) -------------------------------
# Built with SSE (like DOOM/Quake) so it can use float for the ray geometry; the
# generic user rule uses -mgeneral-regs-only (no float), so give it its own rule.
$(BUILD)/raycast.elf: user/raycast.c $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -w \
	      -msse2 -mfpmath=sse -Ikernel/include -c user/raycast.c -o $(BUILD)/raycast_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/raycast_app.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (Raycaster)"

# --- Asteroids (vector arcade; float physics, so SSE like the raycaster) ------
$(BUILD)/asteroids.elf: user/asteroids.c $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -w \
	      -msse2 -mfpmath=sse -Ikernel/include -c user/asteroids.c -o $(BUILD)/asteroids_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/asteroids_app.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (Asteroids)"

# --- Lunar Lander (float physics, so SSE like the raycaster/asteroids) -------
$(BUILD)/lander.elf: user/lander.c $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -w \
	      -msse2 -mfpmath=sse -Ikernel/include -c user/lander.c -o $(BUILD)/lander_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/lander_app.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (Lunar Lander)"

$(BUILD)/missile.elf: user/missile.c $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -w \
	      -msse2 -mfpmath=sse -Ikernel/include -c user/missile.c -o $(BUILD)/missile_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/missile_app.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (Missile Command)"

# --- calc (scientific calculator: now floating point, so SSE like the games) --
# The generic user rule uses -mgeneral-regs-only (no float); the float evaluator
# (user/calceval.h + user/dmath.h) needs SSE, so give calc its own rule. Keep
# -fwrapv + -Wall so the OS-authored evaluator (calceval.h/calc.c) stays under
# full warnings. The two -Wno- flags target ONLY user/dmath.h, which is copied
# verbatim from kernel/js.c (already-tested math): -Wmisleading-indentation
# fires on its one-line i64_to_str (js.c is itself non-clean under -Wextra), and
# -Wstringop-overflow is a known GCC false positive on num_to_str's digit loop
# (provably tn<=15<=sizeof tmp) seen only after inlining. calceval.h/calc.c are
# warning-clean on their own. Object name (user_calc.o) matches the build.
$(BUILD)/calc.elf: user/calc.c user/calceval.h user/dmath.h $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -fwrapv -Wall \
	      -Wno-misleading-indentation -Wno-stringop-overflow \
	      -msse2 -mfpmath=sse -Ikernel/include -c user/calc.c -o $(BUILD)/user_calc.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/user_calc.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (scientific calculator)"

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

# Same as `run`, but with a Realtek RTL8139 NIC instead of the e1000 — boots the
# whole stack over the second card driver (kernel/rtl8139.c) so you can watch the
# serial log say "rtl8139 up" and ARP/ping/HTTP the SLIRP gateway over it.
run-rtl8139: $(KERNEL) $(DISK)
	$(QEMU) $(QEMUFLAGS) -kernel $(KERNEL) $(DISKFLAGS) $(RTLNICFLAGS) $(USBFLAGS) $(AUDIOFLAGS) -serial stdio

# Same as `run`, but with a LEGACY virtio-net NIC instead of the e1000 — boots the
# whole stack over the paravirtual NIC driver (kernel/virtio_net.c) so you can
# watch the serial log say "virtio-net up" and ARP/ping/HTTP the SLIRP gateway over it.
run-virtio-net: $(KERNEL) $(DISK)
	$(QEMU) $(QEMUFLAGS) -kernel $(KERNEL) $(DISKFLAGS) $(VIRTIONICFLAGS) $(USBFLAGS) $(AUDIOFLAGS) -serial stdio

# Headless in-guest assertion that the RTL8139 driver brings up the full stack:
# boots with -device rtl8139 (no e1000) and asserts the network markers print
# over it. SKIPs cleanly if QEMU is absent. (Companion to `boottest`, which
# covers the e1000 path.)
rtl8139test: $(KERNEL) $(DISK)
	@tests/run-rtl8139-tests.sh

# Headless in-guest assertion that the virtio-net driver brings up the full stack:
# boots with a LEGACY virtio-net-pci NIC (no e1000) and asserts the network markers
# print over it — the driver bound + read its MAC, ARP resolved the gateway, ICMP
# echo succeeded. SKIPs cleanly if QEMU is absent. (Companion to `boottest`, the
# e1000 path; and to `rtl8139test`, the other concrete NIC.)
virtionettest: $(KERNEL) $(DISK)
	@tests/run-virtio-net-tests.sh

# Headless in-guest assertion for the virtio-blk driver (kernel/virtio_blk.c):
# attaches a SECOND disk over legacy virtio (-device virtio-blk-pci,disable-
# modern=on), boots, and asserts the driver read that disk's KNOWN per-sector
# content back (host-computed checksums + marker) and the write round-trip. The
# boot disk stays on legacy ATA. SKIPs cleanly if QEMU/python3 absent. (Companion
# to `boottest`, which has no virtio disk -> the driver must cleanly no-op there.)
virtioblktest: $(KERNEL) $(DISK)
	@tests/run-virtio-blk-tests.sh

# Headless in-guest assertion for the NVMe driver (kernel/nvme.c): attaches a
# SECOND disk over NVMe (-device nvme + -drive ...,if=none), boots, and asserts
# the driver brought the controller up, IDENTIFYd namespace 1, read that disk's
# KNOWN per-sector content back (host-computed checksums + marker), and the write
# round-trip. The boot disk stays on legacy ATA. SKIPs cleanly if QEMU/python3
# absent. (Companion to `boottest`, which has no NVMe disk -> the driver cleanly
# no-ops there.)
nvmetest: $(KERNEL) $(DISK)
	@tests/run-nvme-tests.sh

# Headless in-guest assertion for ATA multi-drive enumeration + MBR/GPT partition
# parsing (kernel/ata.c + kernel/partition.c): attaches a SECOND IDE disk with an
# MBR + a FAT32 partition (primary slave) and a THIRD with a GPT + a FAT32
# partition (secondary master), boots, and asserts the kernel enumerated all four
# legacy ATA slots, parsed BOTH partition tables correctly (host-computed
# scheme/type/start-LBA/sectors), and read a known file (HELLO.TXT) back from
# each partition's FAT32 at its offset. The boot disk stays on legacy ATA (index
# 0, bare FAT32, no table). SKIPs cleanly if QEMU/python3 absent. (Companion to
# `boottest`, which has no extra disks -> only drive 0 present, no table -> no-op.)
parttest: $(KERNEL) $(DISK)
	@tests/run-partition-tests.sh

# Headless in-guest assertion for the virtio-gpu driver (kernel/virtio_gpu.c):
# attaches a virtio-gpu device (-device virtio-gpu-pci) ALONGSIDE the std-VGA
# display, boots, and asserts the driver brought the modern paravirtual 2D GPU
# up (modern PCI handshake + control virtqueue), read scanout 0's resolution,
# created+attached+scanned-out a backing resource, and ran the full present
# cycle (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH each returned OK) with no fault —
# while the std-VGA boot display path (gfxtest) stays unaffected. SKIPs cleanly
# if QEMU absent. (Companion to boottest/gfxtest, which have no virtio-gpu ->
# the driver must cleanly no-op and the std-VGA desktop must still render.)
virtiogputest: $(KERNEL) $(DISK)
	@tests/run-virtio-gpu-tests.sh

# Same as `run`, but with an Intel HD Audio controller (`intel-hda` + `hda-output`)
# instead of AC'97 — boots the whole desktop over the HDA driver (kernel/hda.c).
# Watch the serial log say "HDA audio: ..." and "audio output: hda"; the jukebox /
# tone / DOOM all play through it.
run-hda: $(KERNEL) $(DISK)
	$(QEMU) $(QEMUFLAGS) -kernel $(KERNEL) $(DISKFLAGS) $(NICFLAGS) $(USBFLAGS) $(HDAAUDIOFLAGS) -serial stdio

# Headless in-guest assertion that the HDA driver brings up an audio output path
# AND its stream DMA actually advances: boots with -device intel-hda + hda-output
# (no AC'97), asserts the controller reset + codec enum + output verbs succeeded
# and the stream's DMA position register advances while a tone plays — with no
# fault. SKIPs cleanly if QEMU is absent. (Companion to `boottest`, the AC'97 path.)
hdatest: $(KERNEL) $(DISK)
	@tests/run-hda-tests.sh

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

# Host-side fuzz of the HTML attribute scanners over untrusted/malformed tag bytes (ASan+UBSan).
htmlattrtest:
	@tests/run-htmlattr-tests.sh

# Host-side fuzz of the URL splitter/resolver over untrusted/malformed URLs (ASan+UBSan).
urltest:
	@tests/run-url-tests.sh

# Host-side fuzz of the CSS colour parser over untrusted/malformed colour tokens (ASan+UBSan).
colortest:
	@tests/run-color-tests.sh

# Host-side fuzz of the inline-style property scanner over untrusted style="" bytes (ASan+UBSan).
csstest:
	@tests/run-css-tests.sh

# Host-side regression + fuzz of the CSS simple-selector parser (kernel/include/cssel.h, ASan+UBSan).
csseltest:
	@tests/run-cssel-tests.sh

# Host-side regression + fuzz of the shell's grep regex matcher (user/shgrep.h, ASan+UBSan).
shgreptest:
	@tests/run-shgrep-tests.sh

# Host-side regression + fuzz of the shell's $((expr)) integer evaluator (user/shmath.h, ASan+UBSan).
shmathtest:
	@tests/run-shmath-tests.sh

# Host-side regression + fuzz of the shell's ';' statement splitter (user/shsplit.h, ASan+UBSan).
shsplittest:
	@tests/run-shsplit-tests.sh

# Host-side regression + fuzz of the shell's brace expansion (user/shbrace.h, ASan+UBSan).
shbracetest:
	@tests/run-shbrace-tests.sh

# Host-side regression + fuzz of the shell's quoting pass (user/shquote.h, ASan+UBSan).
shquotetest:
	@tests/run-shquote-tests.sh

# Host-side regression + fuzz of the calculator app's expression evaluator (user/calceval.h, ASan+UBSan+-fwrapv).
calctest:
	@tests/run-calc-tests.sh

# Host-side regression + fuzz of the shell's cd path resolver (user/normpath.h, ASan+UBSan).
normpathtest:
	@tests/run-normpath-tests.sh

# Host-side regression of the terminal's Tab-completion core (kernel/complete.h, ASan+UBSan).
completetest:
	@tests/run-complete-tests.sh

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

# In-guest BROWSER assertion: launch the Browser from the Apps menu and assert
# its (network-free) home page rendered -- the end-to-end guard for parse_html,
# which is too coupled to fuzz in isolation. SKIPs if QEMU/socat/python3 absent.
browsertest: $(KERNEL) $(DISK)
	@tests/run-browser-tests.sh

# Run every host-side regression/fuzz/KAT suite, then the in-guest boot assertions.
# ('test' above is the human-readable headless boot; 'boottest'/'gfxtest' are asserted.)
check: jstest imgtest x509test nettest fstest kattest svgtest deflatetest pngenctest ziptest tartest heaptest wavtest elftest httptest kheaptest jsonfuzztest regexfuzztest jssrcfuzztest htmlentfuzztest htmlattrtest urltest colortest csstest csseltest shgreptest shmathtest shsplittest shbracetest shquotetest calctest normpathtest completetest boottest rtl8139test virtionettest virtioblktest nvmetest parttest virtiogputest hdatest gfxtest browsertest
	@echo "ALL TESTS PASSED (jstest + imgtest + x509test + nettest + fstest + kattest + svgtest + deflatetest + pngenctest + ziptest + tartest + heaptest + wavtest + elftest + httptest + kheaptest + jsonfuzztest + regexfuzztest + jssrcfuzztest + htmlentfuzztest + htmlattrtest + urltest + colortest + csstest + csseltest + shgreptest + shmathtest + shsplittest + shbracetest + shquotetest + calctest + normpathtest + completetest + boottest + rtl8139test + virtionettest + virtioblktest + nvmetest + parttest + virtiogputest + hdatest + gfxtest + browsertest)"

clean:
	rm -rf $(BUILD)

# Header-dependency tracking: -MMD (in the *CFLAGS) drops a .d next to each .o
# listing every header it included; pulling those in here means editing a header
# rebuilds exactly the objects that include it. Without this, a header edit
# silently shipped a stale .o — e.g. extending shmath.h didn't rebuild shell.o
# until a manual `touch` (M780). Placed last so the included .d rules can't
# hijack the default goal; missing on a clean tree -> ignored.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
