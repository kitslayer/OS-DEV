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
# NOTE: -fstack-protector* is silently a no-op under this toolchain's freestanding
# build (Gentoo GCC strips SSP when -ffreestanding/-nostdlib are set, and
# -mstack-protector-guard=global no-ops too — verified by disassembly). So
# stack-overflow protection is provided structurally instead: kernel stacks get an
# unmapped GUARD PAGE below them (task.c / boot.asm), and W^X marks every stack NX
# (vmm_harden_kernel). Keep -fno-stack-protector explicit so the intent is clear.
CFLAGS  := -std=gnu11 -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -mgeneral-regs-only -fwrapv \
           -fno-omit-frame-pointer \
           -Wall -Wextra -Ikernel/include -O2 -g -MMD -MP

ASFLAGS := -f elf64
LDFLAGS := -n -T linker.ld

# -m 256M: QEMU's ~128M default starves the heaviest app — Quake needs its 18 MB
# PAK + a multi-MB hunk on top of the kernel (incl. the 40 MB JS arena), so it
# silently fails to launch at 128M but runs fine at 256M (DOOM, lighter, works at
# either). 256M comfortably fits the whole app suite, even DOOM+Quake at once.
QEMUFLAGS := -no-reboot -no-shutdown -m 256M -smp 4

# Hardware acceleration for the INTERACTIVE run targets: use KVM (native-speed
# guest — typing / window drags / app launches feel instant, ~10x faster than
# pure emulation) when /dev/kvm is present and writable (the invoking user is in
# the kvm group), else fall back to plain TCG emulation so the build stays
# portable. The headless make-check suites keep their own (TCG) QEMU invocations
# for CI determinism and to run on hosts without KVM.
ACCEL := $(shell test -w /dev/kvm && echo -enable-kvm -cpu host)

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

# kernel/testmod.c is a LOADABLE MODULE (M1261): compiled to an ET_REL .ko and
# incbin'd into the kernel image (kernel/asm/mod_blob.asm), NOT linked in — so
# exclude it from the normal C object list (it would otherwise define mod_init in
# the kernel itself, defeating the point).
C_SRCS  := $(filter-out kernel/testmod.c,$(shell find kernel -name '*.c'))
# ap_trampoline.asm is a FLAT 16-bit real-mode binary (the AP bring-up trampoline,
# M1197) — assembled with `nasm -f bin` and incbin'd by ap_blob.asm, NOT linked
# as an elf64 object — so exclude it from the normal asm object list.
ASM_SRCS:= $(filter-out kernel/asm/ap_trampoline.asm,$(shell find boot kernel -name '*.asm'))
OBJS    := $(patsubst %.c,$(BUILD)/%.o,$(C_SRCS)) \
           $(patsubst %.asm,$(BUILD)/%.o,$(ASM_SRCS))

# --- rules ------------------------------------------------------------------
.PHONY: all run run-rtl8139 run-virtio-net run-hda test rtl8139test virtionettest virtioblktest virtiorngtest virtioconsoletest nvmetest floppytest parttest blockdevtest raidtest idedmatest virtiogputest svgatest usbstoragetest usbkbdtest ehcitest xhcitest hdatest httpdtest jstest clean

all: $(KERNEL) $(DISK)

# --- FAT32 disk image (built by our host-side tool) --------------------------
$(BUILD)/mkfatfs: tools/mkfatfs.c
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -o $@ $<

# A shared library placed on the FAT disk for the userspace dynamic linker
# (M1263): built as a freestanding PIC ET_DYN, loaded at runtime by ulib's
# dlopen()/dlsym(). mkfatfs reads build/dltest.so via its hostfiles[] table, so
# the disk depends on it.
$(BUILD)/dltest.so: user/dltest_lib.c Makefile
	@mkdir -p $(BUILD)
	$(CC) -shared -fPIC -nostdlib -ffreestanding -fno-stack-protector -mno-red-zone -fno-pie -O2 $< -o $@

$(DISK): $(BUILD)/mkfatfs $(BUILD)/dltest.so
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

# A loadable kernel module (M1261): kernel/testmod.c -> an ET_REL object
# build/testmod.ko (same code model as the kernel, so its relocations match what
# kernel/module.c applies). It is NOT linked into the kernel; instead
# kernel/asm/mod_blob.asm incbin's it into .rodata, and insmod loads + relocates
# + runs it. The blob object must be (re)assembled after the .ko exists.
$(BUILD)/testmod.ko: kernel/testmod.c Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/kernel/asm/mod_blob.o: $(BUILD)/testmod.ko

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
USER_ELFS := $(BUILD)/shell.elf $(BUILD)/clock.elf $(BUILD)/calc.elf $(BUILD)/snake.elf $(BUILD)/editor.elf $(BUILD)/g2048.elf $(BUILD)/life.elf $(BUILD)/tetris.elf $(BUILD)/breakout.elf $(BUILD)/mines.elf $(BUILD)/sudoku.elf $(BUILD)/calendar.elf $(BUILD)/timer.elf $(BUILD)/mandel.elf $(BUILD)/piano.elf $(BUILD)/maze.elf $(BUILD)/adv.elf $(BUILD)/matrix.elf $(BUILD)/paint.elf $(BUILD)/hangman.elf $(BUILD)/jukebox.elf $(BUILD)/ttt.elf $(BUILD)/bj.elf $(BUILD)/typing.elf $(BUILD)/simon.elf $(BUILD)/c4.elf $(BUILD)/wordle.elf $(BUILD)/gfxdemo.elf $(BUILD)/scene3d.elf $(BUILD)/terrain.elf $(BUILD)/demoscene.elf $(BUILD)/doom.elf $(BUILD)/quake.elf $(BUILD)/nes.elf $(BUILD)/reversi.elf $(BUILD)/lights.elf $(BUILD)/fifteen.elf $(BUILD)/mastermind.elf $(BUILD)/pong.elf $(BUILD)/halflife.elf $(BUILD)/memory.elf $(BUILD)/sokoban.elf $(BUILD)/battleship.elf $(BUILD)/pig.elf $(BUILD)/raycast.elf $(BUILD)/tron.elf $(BUILD)/spaceinv.elf $(BUILD)/asteroids.elf $(BUILD)/flappy.elf $(BUILD)/gb.elf $(BUILD)/lander.elf $(BUILD)/yahtzee.elf $(BUILD)/checkers.elf $(BUILD)/gomoku.elf $(BUILD)/frogger.elf $(BUILD)/chess.elf $(BUILD)/vpoker.elf $(BUILD)/mancala.elf $(BUILD)/dotsbox.elf $(BUILD)/missile.elf $(BUILD)/pacman.elf $(BUILD)/solitaire.elf $(BUILD)/gems.elf $(BUILD)/columns.elf $(BUILD)/freecell.elf $(BUILD)/spider.elf $(BUILD)/sandbox.elf $(BUILD)/forth.elf $(BUILD)/cc.elf $(BUILD)/crash.elf $(BUILD)/futex.elf $(BUILD)/nettcp.elf $(BUILD)/crashinfo.elf $(BUILD)/forktest.elf $(BUILD)/execdemo.elf $(BUILD)/nstest.elf $(BUILD)/steptest.elf $(BUILD)/scnotify.elf $(BUILD)/fswaittest.elf $(BUILD)/sigfdtest.elf $(BUILD)/bpftest.elf $(BUILD)/fantest.elf $(BUILD)/iouringtest.elf $(BUILD)/msealtest.elf $(BUILD)/httpd.elf $(BUILD)/uffdtest.elf $(BUILD)/mmapfile.elf $(BUILD)/threads.elf $(BUILD)/robustfutex.elf $(BUILD)/overlay.elf $(BUILD)/pcwd.elf $(BUILD)/hexedit.elf $(BUILD)/aclock.elf $(BUILD)/sysgraph.elf $(BUILD)/taskman.elf $(BUILD)/gcal.elf $(BUILD)/gauges.elf $(BUILD)/gsw.elf $(BUILD)/bclock.elf $(BUILD)/gfont.elf $(BUILD)/gtimer.elf $(BUILD)/imgview.elf $(BUILD)/gcalc.elf $(BUILD)/gcolor.elf $(BUILD)/gfire.elf $(BUILD)/gmetro.elf $(BUILD)/gconv.elf $(BUILD)/gbase.elf $(BUILD)/gpass.elf $(BUILD)/gclip.elf $(BUILD)/gtodo.elf $(BUILD)/gseq.elf $(BUILD)/pietest.elf $(BUILD)/jsrun.elf $(BUILD)/imgdec.elf $(BUILD)/httpget.elf $(BUILD)/webview.elf

$(BUILD)/user_%.o: user/%.c Makefile
	@mkdir -p $(BUILD)
	$(CC) $(USER_CFLAGS) -c $< -o $@

# cc.c is the vendored c4 C compiler — terse C that's noisy under -Wall; -w it.
$(BUILD)/user_cc.o: user/cc.c Makefile
	@mkdir -p $(BUILD)
	$(CC) $(USER_CFLAGS) -w -c user/cc.c -o $@

# crash.c keeps frame pointers so its core dump (M1104) has a walkable saved-rbp
# chain — i.e. so `crashinfo` (M1112) can produce a real frame-pointer backtrace.
$(BUILD)/user_crash.o: user/crash.c Makefile
	@mkdir -p $(BUILD)
	$(CC) $(USER_CFLAGS) -fno-omit-frame-pointer -c user/crash.c -o $@

$(BUILD)/%.elf: $(BUILD)/user_%.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld
	$(LD) -T user/user.ld -o $@ $(BUILD)/user_$*.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (userspace program)"

# pietest.elf — a POSITION-INDEPENDENT executable (M1465): built -fPIE -pie so it
# is an ET_DYN carrying R_X86_64_RELATIVE relocs, the test for the kernel's new
# elf_load_dyn path. This SPECIFIC rule overrides the %.elf pattern above; the
# program is self-contained (its own _start + inline syscalls, no ulib) so its
# only reloc is RELATIVE — exactly what the loader handles.
$(BUILD)/pietest.elf: user/pietest.c Makefile
	@mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -fPIE -pie -mno-red-zone -mcmodel=small -std=gnu11 -O2 -w -e _start -o $@ user/pietest.c
	@echo "Built $@ (PIE test executable)"

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

# --- jsrun (the from-scratch JavaScript engine, running in RING 3) ------------
# First step of moving the browser/parser stack out of ring 0: kernel/js.c is pure
# compute (arena + libc + callback I/O), so it links into a userspace program built
# with SSE (the engine uses IEEE-754 doubles), -DJS_RING3 (drops the privileged
# cli/sti single-flight guard, which would #GP in ring 3) and a 16 MB arena. A bug
# in the 4600-line interpreter now crashes only this ring-3 process, not the kernel.
$(BUILD)/jsrun.elf: user/jsrun.c kernel/js.c $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -w \
	      -msse2 -mfpmath=sse -Ikernel/include -DJS_RING3 -DJS_ARENA=16777216 \
	      -c kernel/js.c -o $(BUILD)/jsrun_js.o
	$(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -Wall \
	      -msse2 -mfpmath=sse -Ikernel/include -c user/jsrun.c -o $(BUILD)/jsrun_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/jsrun_app.o $(BUILD)/jsrun_js.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (ring-3 JavaScript engine)"

# --- imgdec (the from-scratch image decoders, running in RING 3) --------------
# Second step of the ring-0->ring-3 move (after jsrun): PNG/GIF/JPEG/SVG/BMP +
# inflate are pure compute over caller-provided buffers (already host-fuzzed in
# tests/), so they link straight into a ring-3 program. A malformed-image bug now
# crashes only this process, not the kernel. SSE on in case a decoder uses float.
IMGDEC_CC = $(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -msse2 -mfpmath=sse -Ikernel/include
$(BUILD)/imgdec.elf: user/imgdec.c kernel/png.c kernel/gif.c kernel/jpeg.c kernel/bmp.c kernel/svg.c kernel/inflate.c $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(IMGDEC_CC) -w -c kernel/png.c     -o $(BUILD)/imgdec_png.o
	$(IMGDEC_CC) -w -c kernel/gif.c     -o $(BUILD)/imgdec_gif.o
	$(IMGDEC_CC) -w -c kernel/jpeg.c    -o $(BUILD)/imgdec_jpeg.o
	$(IMGDEC_CC) -w -c kernel/bmp.c     -o $(BUILD)/imgdec_bmp.o
	$(IMGDEC_CC) -w -c kernel/svg.c     -o $(BUILD)/imgdec_svg.o
	$(IMGDEC_CC) -w -c kernel/inflate.c -o $(BUILD)/imgdec_inflate.o
	$(IMGDEC_CC) -w -c kernel/font.c    -o $(BUILD)/imgdec_font.o
	$(IMGDEC_CC) -Wall -c user/imgdec.c -o $(BUILD)/imgdec_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/imgdec_app.o $(BUILD)/imgdec_png.o $(BUILD)/imgdec_gif.o $(BUILD)/imgdec_jpeg.o $(BUILD)/imgdec_bmp.o $(BUILD)/imgdec_svg.o $(BUILD)/imgdec_inflate.o $(BUILD)/imgdec_font.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (ring-3 image decoders)"

# --- imgview now DECODES IN RING 3: the graphical image viewer links the
# from-scratch decoders directly (like imgdec) and fit-scales in-process, instead
# of the in-kernel sys_loadimg — so a malformed image opened in the viewer crashes
# only imgview, not the kernel. Overrides the generic %.elf rule. ---
$(BUILD)/imgview.elf: user/imgview.c kernel/png.c kernel/gif.c kernel/jpeg.c kernel/bmp.c kernel/svg.c kernel/inflate.c kernel/font.c $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(IMGDEC_CC) -w -c kernel/png.c     -o $(BUILD)/imgview_png.o
	$(IMGDEC_CC) -w -c kernel/gif.c     -o $(BUILD)/imgview_gif.o
	$(IMGDEC_CC) -w -c kernel/jpeg.c    -o $(BUILD)/imgview_jpeg.o
	$(IMGDEC_CC) -w -c kernel/bmp.c     -o $(BUILD)/imgview_bmp.o
	$(IMGDEC_CC) -w -c kernel/svg.c     -o $(BUILD)/imgview_svg.o
	$(IMGDEC_CC) -w -c kernel/inflate.c -o $(BUILD)/imgview_inflate.o
	$(IMGDEC_CC) -w -c kernel/font.c    -o $(BUILD)/imgview_font.o
	$(IMGDEC_CC) -Wall -c user/imgview.c -o $(BUILD)/imgview_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/imgview_app.o $(BUILD)/imgview_png.o $(BUILD)/imgview_gif.o $(BUILD)/imgview_jpeg.o $(BUILD)/imgview_bmp.o $(BUILD)/imgview_svg.o $(BUILD)/imgview_inflate.o $(BUILD)/imgview_font.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (ring-3 image viewer)"

# --- webview (the from-scratch web browser, running in RING 3) -----------------
# Headline ring-0 -> ring-3 migration: browser.c + js.c + image decoders + HTML/
# CSS/URL parsers built for ring 3 (-DBROWSER_RING3 / -DJS_RING3), so the untrusted
# HTML/CSS/JS/image parsing runs OUTSIDE the kernel. Network/TLS stay in the kernel
# (fetch via SYS_http/SYS_https syscalls, shimmed in webview.c). A parser bug now
# crashes only this ring-3 process. SSE: js.c uses IEEE-754 doubles.
WEBVIEW_CC = $(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -msse2 -mfpmath=sse -Ikernel/include -w
WEBVIEW_PARSERS = png gif jpeg bmp svg webp inflate font http cssprop color url htmlentity htmlattr reader
$(BUILD)/webview.elf: user/webview.c kernel/browser.c kernel/js.c $(patsubst %,kernel/%.c,$(WEBVIEW_PARSERS)) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(WEBVIEW_CC) -DBROWSER_RING3 -c kernel/browser.c -o $(BUILD)/webview_browser.o
	$(WEBVIEW_CC) -DJS_RING3 -DJS_ARENA=16777216 -c kernel/js.c -o $(BUILD)/webview_js.o
	@for m in $(WEBVIEW_PARSERS); do echo "  CC kernel/$$m.c (ring-3 browser)"; $(WEBVIEW_CC) -c kernel/$$m.c -o $(BUILD)/webview_$$m.o || exit 1; done
	$(WEBVIEW_CC) -c user/webview.c -o $(BUILD)/webview_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/webview_app.o $(BUILD)/webview_browser.o $(BUILD)/webview_js.o $(patsubst %,$(BUILD)/webview_%.o,$(WEBVIEW_PARSERS)) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (ring-3 web browser)"

# --- httpget (the from-scratch TLS 1.3 client, running in RING 3) -------------
# Third step of the ring-0->ring-3 move: tls.c + its crypto (X25519/ECDSA/RSA/
# AES-GCM/ChaCha20-Poly1305/SHA-2/X.509) are static-buffer pure compute (host-
# fuzzed in tests/), so they link into a ring-3 program; httpget.c shims kernel
# tcp_*/DNS/clock onto the socket/resolve/time syscalls. -DTLS_RING3 drops tls.c's
# privileged cli/sti. Integer-only crypto -> -mgeneral-regs-only (as the kernel).
HTTPGET_CC = $(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only -std=gnu11 -O2 -Ikernel/include
HTTPGET_CRYPTO = aes aesgcm bignum chachapoly ecdsa hkdf rsa sha256 sha512 x25519 x509 rootca
$(BUILD)/httpget.elf: user/httpget.c kernel/tls.c $(patsubst %,kernel/%.c,$(HTTPGET_CRYPTO)) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(HTTPGET_CC) -w -DTLS_RING3 -c kernel/tls.c -o $(BUILD)/httpget_tls.o
	@for m in $(HTTPGET_CRYPTO); do echo "  CC kernel/$$m.c (ring-3 crypto)"; extra=""; [ "$$m" = "aes" ] && extra="-DAES_RING3"; $(HTTPGET_CC) -w $$extra -c kernel/$$m.c -o $(BUILD)/httpget_$$m.o || exit 1; done
	$(HTTPGET_CC) -Wall -c user/httpget.c -o $(BUILD)/httpget_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/httpget_app.o $(BUILD)/httpget_tls.o $(patsubst %,$(BUILD)/httpget_%.o,$(HTTPGET_CRYPTO)) $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (ring-3 TLS 1.3 client)"

# --- scene3d (software 3D engine: z-buffer + Gouraud, float math, so SSE) -----
$(BUILD)/scene3d.elf: user/scene3d.c $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -w \
	      -msse2 -mfpmath=sse -Ikernel/include -c user/scene3d.c -o $(BUILD)/scene3d_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/scene3d_app.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (3D Engine)"

# --- terrain (procedural heightmap flythrough: z-buffer + fog, float, so SSE) -
$(BUILD)/terrain.elf: user/terrain.c $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o user/user.ld Makefile
	@mkdir -p $(BUILD)
	$(CC) -ffreestanding -nostdlib -fno-pic -fno-pie -mno-red-zone -std=gnu11 -O2 -w \
	      -msse2 -mfpmath=sse -Ikernel/include -c user/terrain.c -o $(BUILD)/terrain_app.o
	$(LD) -T user/user.ld -o $@ $(BUILD)/terrain_app.o $(BUILD)/user_ulib.o $(BUILD)/user_umalloc.o
	@echo "Built $@ (Terrain)"

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

# AP bring-up trampoline (M1197): assemble the real-mode trampoline as a flat
# binary, then make the embedding blob (ap_blob.asm incbin's build/ap_trampoline.bin)
# depend on it so it's built first.
$(BUILD)/ap_trampoline.bin: kernel/asm/ap_trampoline.asm
	@mkdir -p $(BUILD)
	$(AS) -f bin $< -o $@
$(BUILD)/kernel/asm/ap_blob.o: $(BUILD)/ap_trampoline.bin

# kernel.elf is built in TWO link passes so the embedded symbol table (for panic
# backtraces — kernel/ksyms.c) holds the FINAL function addresses:
#   pass 1 links with a zero-entry stub table to learn the addresses; we then run
#   `nm` + tools/gen_ksyms.sh to emit the real table and relink. The table lands
#   in .rodata (after .text), so adding it never shifts a function address —
#   making the pass-1 addresses exact for pass 2.
KSYMSC  := $(BUILD)/ksyms_table.c
KSYMSO  := $(BUILD)/ksyms_table.o
$(KERNEL64): $(OBJS) linker.ld tools/gen_ksyms.sh
	@mkdir -p $(dir $@)
	@printf '#include "ksyms.h"\nconst struct ksym ksyms[]={{0,0}};\nconst int ksyms_count=0;\n' > $(KSYMSC)
	$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -c $(KSYMSC) -o $(KSYMSO)
	$(LD) $(LDFLAGS) -o $(BUILD)/kernel_pass1.elf $(OBJS) $(KSYMSO)
	@nm -n $(BUILD)/kernel_pass1.elf | sh tools/gen_ksyms.sh > $(KSYMSC)
	$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -c $(KSYMSC) -o $(KSYMSO)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(KSYMSO)

$(KERNEL): $(KERNEL64)
	$(OBJCOPY) -I elf64-x86-64 -O elf32-i386 $< $@
	@echo "Built $@ (64-bit kernel in a multiboot-loadable 32-bit container)"

# Interactive: opens a QEMU window so you can see the VGA output.
run: $(KERNEL) $(DISK)
	$(QEMU) $(QEMUFLAGS) $(ACCEL) -kernel $(KERNEL) $(DISKFLAGS) $(NICFLAGS) $(USBFLAGS) $(AUDIOFLAGS) -serial stdio

# --- bare metal (see BAREMETAL.md) ------------------------------------------
# Boot the Multiboot kernel via REAL GRUB (not QEMU's -kernel shortcut) — the
# path a physical machine uses. The kernel requests + consumes a Multiboot
# framebuffer (boot.asm + fb_init_mb), falling back to Bochs std-VGA under QEMU.

# BIOS GRUB rescue ISO. Needs xorriso (Gentoo: emerge dev-libs/libisoburn).
# Verify the GRUB->Multiboot handoff: qemu-system-x86_64 -cdrom build/os.iso
# (no -kernel). Deploy to a USB stick: dd if=build/os.iso of=/dev/sdX bs=4M.
iso: $(KERNEL)
	@command -v xorriso >/dev/null || { echo "iso: needs xorriso — emerge dev-libs/libisoburn"; exit 1; }
	@mkdir -p $(BUILD)/isodir/boot/grub
	cp $(KERNEL) $(BUILD)/isodir/boot/kernel32.elf
	printf 'set timeout=0\nset default=0\nmenuentry "OS-DEV" {\n\tmultiboot2 /boot/kernel32.elf\n\tboot\n}\n' > $(BUILD)/isodir/boot/grub/grub.cfg
	grub-mkrescue -d /usr/lib/grub/i386-pc -o $(BUILD)/os.iso $(BUILD)/isodir
	@echo "Built $(BUILD)/os.iso — verify: qemu-system-x86_64 -cdrom $(BUILD)/os.iso -serial stdio"

# UEFI standalone GRUB image (no xorriso needed — VERIFIED to boot under OVMF).
# Embeds the kernel in GRUB's memdisk. Deploy: copy build/BOOTX64.EFI to
# <USB>/EFI/BOOT/BOOTX64.EFI on a FAT-formatted stick and boot a UEFI machine.
efi: $(KERNEL)
	printf 'set timeout=0\nset default=0\ninsmod all_video\nmenuentry "OS-DEV" {\n\tmultiboot2 /boot/kernel32.elf\n\tboot\n}\n' > $(BUILD)/grub-efi.cfg
	grub-mkstandalone -O x86_64-efi -o $(BUILD)/BOOTX64.EFI \
	    --modules="multiboot2 normal all_video efi_gop efi_uga part_gpt fat" \
	    "boot/grub/grub.cfg=$(BUILD)/grub-efi.cfg" "boot/kernel32.elf=$(KERNEL)"
	@echo "Built $(BUILD)/BOOTX64.EFI — copy to <USB>/EFI/BOOT/BOOTX64.EFI on a FAT ESP"

# Same as `run`, but with a Realtek RTL8139 NIC instead of the e1000 — boots the
# whole stack over the second card driver (kernel/rtl8139.c) so you can watch the
# serial log say "rtl8139 up" and ARP/ping/HTTP the SLIRP gateway over it.
run-rtl8139: $(KERNEL) $(DISK)
	$(QEMU) $(QEMUFLAGS) $(ACCEL) -kernel $(KERNEL) $(DISKFLAGS) $(RTLNICFLAGS) $(USBFLAGS) $(AUDIOFLAGS) -serial stdio

# Same as `run`, but with a LEGACY virtio-net NIC instead of the e1000 — boots the
# whole stack over the paravirtual NIC driver (kernel/virtio_net.c) so you can
# watch the serial log say "virtio-net up" and ARP/ping/HTTP the SLIRP gateway over it.
run-virtio-net: $(KERNEL) $(DISK)
	$(QEMU) $(QEMUFLAGS) $(ACCEL) -kernel $(KERNEL) $(DISKFLAGS) $(VIRTIONICFLAGS) $(USBFLAGS) $(AUDIOFLAGS) -serial stdio

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

# Headless in-guest assertion for the virtio-rng driver (kernel/virtio_rng.c):
# attaches a virtio entropy device (-device virtio-rng-pci), boots, and asserts
# the driver brought it up and DMA'd real entropy in — the boot self-test draws
# two batches over the virtqueue and proves them nonzero + differing. The boot
# disk stays on legacy ATA. SKIPs cleanly if QEMU absent. (Companion to
# `boottest`, which has no virtio-rng device -> the driver must cleanly no-op.)
virtiorngtest: $(KERNEL) $(DISK)
	@tests/run-virtio-rng-tests.sh

# Headless in-guest assertion for the virtio-console driver (kernel/virtio_console.c):
# attaches a virtio-serial bus + a console port whose host end is a FILE, boots,
# and asserts the boot self-test's transmit-queue write actually reached that
# host file (guest->host data path), with the desktop reached and no fault. SKIPs
# cleanly if QEMU absent. (Companion to `boottest`, which has no virtio-console
# -> the driver must cleanly no-op.)
virtioconsoletest: $(KERNEL) $(DISK)
	@tests/run-virtio-console-tests.sh

# Headless in-guest assertion for the NVMe driver (kernel/nvme.c): attaches a
# SECOND disk over NVMe (-device nvme + -drive ...,if=none), boots, and asserts
# the driver brought the controller up, IDENTIFYd namespace 1, read that disk's
# KNOWN per-sector content back (host-computed checksums + marker), and the write
# round-trip. The boot disk stays on legacy ATA. SKIPs cleanly if QEMU/python3
# absent. (Companion to `boottest`, which has no NVMe disk -> the driver cleanly
# no-ops there.)
nvmetest: $(KERNEL) $(DISK)
	@tests/run-nvme-tests.sh

# Headless in-guest assertion for the floppy controller driver (kernel/floppy.c):
# attaches a 1.44 MB floppy image (-drive ...,if=floppy), boots, and asserts the
# 82077AA driver RESET + RECALIBRATEd the controller and read that diskette's
# KNOWN per-sector content back over ISA DMA (host-computed checksums + marker),
# including a multi-sector read. Unlike the other DMA drivers (PCI bus-master),
# the floppy moves data through the legacy 8237 ISA DMA controller (channel 2 +
# a low-RAM, 64-KiB-bounded bounce buffer). The boot disk stays on legacy ATA.
# SKIPs cleanly if QEMU/python3 absent. (Companion to `boottest`, which has no
# floppy -> the driver must cleanly no-op there.)
floppytest: $(KERNEL) $(DISK)
	@tests/run-floppy-tests.sh

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

# Headless in-guest assertion for the generic block-device layer + read-only
# multi-volume FAT32 browsing over ALL storage drivers (kernel/blockdev.c + the
# device-agnostic fatvol_list/fatvol_find in kernel/partition.c): attaches a
# SECOND disk over LEGACY virtio-blk whose content is a bare FAT32 filesystem with
# KNOWN root files (GREET.TXT, NUMBERS.DAT) + a subdir, boots, and asserts
# blockdev_enumerate() registered the virtio-blk device, MOUNTED its FAT32 volume
# read-only, and LISTED those known entries (names + sizes) -> proving the disk is
# browsable over a non-ATA driver, not just self-tested. The boot disk stays on
# legacy ATA (bare FAT32) and the boot mount + fat32.c/vfs.c are untouched (this
# layer is additive + read-only). SKIPs cleanly if QEMU/python3 absent. (Companion
# to `boottest`, which has only the ATA boot disk -> it alone is registered + its
# bare FAT32 listed, a clean no-fault path.)
blockdevtest: $(KERNEL) $(DISK)
	@tests/run-blockdev-tests.sh

# Headless in-guest assertion for software RAID (device-mapper, kernel/dm.c):
# attaches 3 non-boot SATA disks on an AHCI HBA so dm_selftest builds RAID-1/0/5
# volumes and proves mirror redundancy, stripe distribution, and RAID-5 parity
# reconstruction (single-disk fault tolerance). Boot stays on legacy ATA; the
# self-test is a no-op without extra disks, so boottest is unaffected.
raidtest: $(KERNEL) $(DISK)
	@tests/run-raid-tests.sh

# In-guest HTTP SERVER test (M1304): boots with a host->guest TCP port forward,
# runs the `httpd` shell command (net_tcp_serve), then curls the forwarded port
# from the HOST and asserts the served page comes back -- the inbound-connection
# counterpart of boottest's outbound client. Needs socat + curl on the host.
httpdtest: $(KERNEL) $(DISK)
	@tests/run-httpd-tests.sh

# Headless in-guest assertion for the bus-master IDE DMA path (kernel/ata.c):
# boots with the NORMAL IDE boot disk (-drive ...,if=ide, on the bus-master-capable
# PIIX3 IDE controller) and asserts the kernel's DMA read path returned BYTE-
# IDENTICAL data to the trusted PIO path -- ata_dma_selftest() DMA-reads a few of
# the boot disk's sectors, PIO-reads the same, and logs "DMA==PIO OK" per sector
# (plus a DMA write round-trip). The boot path (PIO ata_read/ata_write) is
# untouched: FAT32 still mounts on legacy ATA via PIO. SKIPs cleanly if QEMU is
# absent. (Companion to `boottest`, which uses the same disk but doesn't assert
# the DMA==PIO comparison.)
idedmatest: $(KERNEL) $(DISK)
	@tests/run-ide-dma-tests.sh

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

# Headless in-guest assertion for the VMware SVGA-II driver (kernel/svga.c):
# attaches a VMware SVGA-II device (-device vmware-svga) ALONGSIDE the std-VGA
# display, boots, and asserts the driver confirmed SVGA_ID_2 over the I/O-port
# index/value register file, read the linear framebuffer (BAR1) + command FIFO
# (BAR2) addresses+sizes, set a mode, wrote a colour-band test pattern to the
# framebuffer, emitted an SVGA_CMD_UPDATE into the FIFO + synced, and read the
# registers back OK with no fault -- while the std-VGA boot display path
# (gfxtest) stays unaffected. SKIPs cleanly if QEMU (or the vmware-svga device)
# is absent. NOTE: distinct from `svgtest` (the SVG image rasterizer fuzz test).
svgatest: $(KERNEL) $(DISK)
	@tests/run-svga-tests.sh

# Headless in-guest assertion for the USB mass-storage driver (kernel/usb_storage.c):
# attaches a USB flash disk as a usb-storage device ON THE SAME UHCI BUS as the
# existing usb-tablet (-device usb-storage,bus=uhci.0,port=2 + -drive ...,if=none),
# boots, and asserts the driver enumerated the Bulk-Only-Transport / SCSI device
# (class 08 / subclass 06 / proto 50 + its bulk endpoints), READ CAPACITYd it,
# read that disk's KNOWN per-sector content back over BOT (host-computed checksums
# + marker), and the WRITE(10) round-trip. The boot disk stays on legacy ATA and
# the USB tablet stays up (shared controller). SKIPs cleanly if QEMU/python3
# absent. (Companion to `boottest`, which has no usb-storage -> the driver must
# cleanly no-op there and the tablet must still come up.)
usbstoragetest: $(KERNEL) $(DISK)
	@tests/run-usb-storage-tests.sh

# Headless in-guest assertion for the USB HID boot-keyboard driver (kernel/usb_kbd.c):
# attaches a usb-kbd ON THE SAME UHCI BUS as the existing usb-tablet (-device
# usb-kbd,bus=uhci.0,port=2), boots, and asserts the driver enumerated the HID
# boot keyboard (class 03 / subclass 01 = Boot / proto 01 = Keyboard + its
# interrupt-IN endpoint), selected boot protocol (HID SET_PROTOCOL(0) ok), and
# DECODED keystrokes injected via QEMU's `sendkey` monitor command over the
# interrupt endpoint. The PS/2 keyboard stays the primary input and the USB tablet
# stays up (shared controller). SKIPs cleanly if QEMU/socat absent. (Companion to
# `boottest`, which has no usb-kbd -> the driver must cleanly no-op there and the
# tablet + PS/2 keyboard must still come up.)
usbkbdtest: $(KERNEL) $(DISK)
	@tests/run-usb-kbd-tests.sh

# Headless in-guest assertion for the EHCI (USB 2.0) host-controller driver
# (kernel/ehci.c): attaches an EHCI controller (-device usb-ehci) WITH a usb-storage
# flash disk on ITS bus (-device usb-storage,bus=ehci.0) — a SEPARATE, additional
# USB host alongside the existing UHCI controller + tablet — boots, and asserts the
# driver brought the USB 2.0 controller up (PCI class 0C/03/20 probe + HC reset +
# async QH/qTD schedule running), reset+enabled a high-speed root port, ENUMERATED
# the device over EHCI control transfers (device/config descriptors read, address
# assigned, SET_CONFIGURATION), and — the stretch bulk path — read SECTOR 0 over
# EHCI bulk (BOT/SCSI READ(10)) returning that disk's KNOWN content (host-computed
# checksum + marker). The UHCI tablet stays up and boot stays on legacy ATA. SKIPs
# cleanly if QEMU/python3 absent. (Companion to `boottest`, which has no EHCI
# controller -> the driver must cleanly no-op there and UHCI + the tablet stay up.)
ehcitest: $(KERNEL) $(DISK)
	@tests/run-ehci-tests.sh

# Headless in-guest assertion for the xHCI (USB 3.0) host-controller driver
# (kernel/xhci.c): attaches an xHCI controller (-device qemu-xhci) WITH a usb-storage
# flash disk on ITS bus (-device usb-storage,bus=xhci.0) — a SEPARATE, additional
# USB host alongside the existing UHCI controller + tablet AND the EHCI controller —
# boots, and asserts the driver brought the USB 3.0 controller up (PCI class 0C/03/30
# probe + HC reset + command/event TRB rings running), that the rings work end to end
# (ENABLE SLOT returned a slot id), reset+detected a root port, ENUMERATED the device
# over xHCI control transfers (ENABLE SLOT + ADDRESS DEVICE, device/config descriptors
# read over the EP0 TRB ring, SET_CONFIGURATION), and — the stretch bulk path — read
# SECTOR 0 over xHCI bulk (BOT/SCSI READ(10)) returning that disk's KNOWN content
# (host-computed checksum + marker). The UHCI tablet + EHCI stay up and boot stays on
# legacy ATA. SKIPs cleanly if QEMU/python3 absent (or the qemu-xhci device is missing).
# (Companion to `boottest`, which has no xHCI controller -> the driver must cleanly
# no-op there and UHCI + EHCI + the tablet stay up.)
xhcitest: $(KERNEL) $(DISK)
	@tests/run-xhci-tests.sh

# Same as `run`, but with an Intel HD Audio controller (`intel-hda` + `hda-output`)
# instead of AC'97 — boots the whole desktop over the HDA driver (kernel/hda.c).
# Watch the serial log say "HDA audio: ..." and "audio output: hda"; the jukebox /
# tone / DOOM all play through it.
run-hda: $(KERNEL) $(DISK)
	$(QEMU) $(QEMUFLAGS) $(ACCEL) -kernel $(KERNEL) $(DISKFLAGS) $(NICFLAGS) $(USBFLAGS) $(HDAAUDIOFLAGS) -serial stdio

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

ext2test:
	@tests/run-ext2-tests.sh

xattrtest:
	@tests/run-xattr-tests.sh

iso9660test:
	@tests/run-iso9660-tests.sh

# Host-side known-answer test of the crypto primitives vs published RFC/FIPS vectors (ASan+UBSan).
kattest:
	@tests/run-crypto-tests.sh

# Host-side property fuzz of bn_mod vs Python's arbitrary-precision integers, 1..8320-bit (ASan+UBSan).
bignumfuzztest:
	@tests/run-bignum-fuzz-tests.sh

# Host-side differential fuzz of memcpy/memset/memmove vs a naive reference (every overlap shape, ASan+UBSan).
stringtest:
	@tests/run-string-tests.sh

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

# Host-side regression + fuzz of the reader-mode content extractor (kernel/reader.c, ASan+UBSan).
readertest:
	@tests/run-reader-tests.sh

# Host-side regression + fuzz of the shell's grep regex matcher (user/shgrep.h, ASan+UBSan).
shgreptest:
	@tests/run-shgrep-tests.sh

# Host-side regression + fuzz of the shell's sed substitution engine (user/shsed.h, ASan+UBSan).
shsedtest:
	@tests/run-shsed-tests.sh

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

# In-guest assertion that a kernel stack overflow is caught + diagnosed by the
# guard page (M1495), with a working high-VA backtrace (M1496) -- not the silent
# heap corruption it used to be (M1491). Boots `-append kstackover` (M1498); the
# companion boottest boots WITHOUT the flag, so the deliberate overflow never
# fires there. The only suite that triggers a real kernel fault end-to-end.
kstacktest: $(KERNEL) $(DISK)
	@tests/run-kstack-test.sh

# In-guest assertion that a RING-3 user-stack overflow is caught by the M1499
# guard page -- it faults cleanly on the guard (CR2 ~ USTACK_BASE 0x50000000) and
# kills only the app, leaving the kernel running. Boots `-append ustackover`
# (M1500), which spawns `crash stack` (user/crash.c).
ustacktest: $(KERNEL) $(DISK)
	@tests/run-ustack-test.sh

# In-guest assertion that W^X/NX is enforced (not just configured): booting
# `-append wxtest` (M1501) makes the kernel try to execute a byte in a .bss
# buffer; it must take an instruction-fetch #PF instead of running -- the headline
# anti-code-injection guarantee. Proves vmm_harden_kernel's NX is real.
wxtest: $(KERNEL) $(DISK)
	@tests/run-wx-test.sh

# In-guest assertion that SMEP is enforced: booting `-append smeptest` (M1502, on
# a SMEP-capable `-cpu qemu64,+smep` model) makes the kernel try to execute a user
# (PTE_USER) page; it must fault instead of running -- the ret2user defence.
smeptest: $(KERNEL) $(DISK)
	@tests/run-smep-test.sh

# gdbstubtest (M1204): boot `-append gdbstub`, attach real host gdb over COM2,
# assert it reads + symbolizes registers. Skips if gdb/qemu are unavailable.
gdbstubtest: $(KERNEL)
	@tests/run-gdbstub-test.sh

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
check: jstest imgtest x509test nettest fstest ext2test xattrtest iso9660test kattest bignumfuzztest stringtest svgtest deflatetest pngenctest ziptest tartest heaptest wavtest elftest httptest kheaptest jsonfuzztest regexfuzztest jssrcfuzztest htmlentfuzztest htmlattrtest urltest colortest csstest csseltest readertest shgreptest shsedtest shmathtest shsplittest shbracetest shquotetest calctest normpathtest completetest boottest kstacktest ustacktest wxtest smeptest gdbstubtest rtl8139test virtionettest virtioblktest virtiorngtest virtioconsoletest nvmetest floppytest parttest blockdevtest raidtest idedmatest virtiogputest svgatest usbstoragetest usbkbdtest ehcitest xhcitest hdatest httpdtest gfxtest browsertest
	@echo "ALL TESTS PASSED (jstest + imgtest + x509test + nettest + fstest + ext2test + xattrtest + iso9660test + kattest + bignumfuzztest + stringtest + svgtest + deflatetest + pngenctest + ziptest + tartest + heaptest + wavtest + elftest + httptest + kheaptest + jsonfuzztest + regexfuzztest + jssrcfuzztest + htmlentfuzztest + htmlattrtest + urltest + colortest + csstest + csseltest + readertest + shgreptest + shsedtest + shmathtest + shsplittest + shbracetest + shquotetest + calctest + normpathtest + completetest + boottest + kstacktest + ustacktest + wxtest + smeptest + gdbstubtest + rtl8139test + virtionettest + virtioblktest + virtiorngtest + virtioconsoletest + nvmetest + floppytest + parttest + blockdevtest + raidtest + idedmatest + virtiogputest + svgatest + usbstoragetest + usbkbdtest + ehcitest + xhcitest + hdatest + httpdtest + gfxtest + browsertest)"

clean:
	rm -rf $(BUILD)

# Header-dependency tracking: -MMD (in the *CFLAGS) drops a .d next to each .o
# listing every header it included; pulling those in here means editing a header
# rebuilds exactly the objects that include it. Without this, a header edit
# silently shipped a stale .o — e.g. extending shmath.h didn't rebuild shell.o
# until a manual `touch` (M780). Placed last so the included .d rules can't
# hijack the default goal; missing on a clean tree -> ignored.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
