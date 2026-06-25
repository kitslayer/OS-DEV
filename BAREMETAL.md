# Running OS-DEV on bare metal

OS-DEV normally boots via QEMU's `-kernel` shortcut (`make run`). This document
covers booting it through a **real bootloader (GRUB)** — the path a physical
machine uses — and the current status / remaining work.

## What works today

- **Valid Multiboot1 kernel.** `boot/boot.asm` has a Multiboot1 header (magic
  `0x1BADB002`) and now also **requests a linear framebuffer** (flag bit 2 +
  the `1280x960x32` video fields). The 32→64-bit long-mode trampoline, the real
  Multiboot memory map (`pmm.c`), ACPI/APIC/SMP bring-up, and the ATA/AHCI/NVMe/
  USB storage stack are all real-hardware-oriented.
- **Boots under real GRUB.** Verified end-to-end under QEMU + OVMF (UEFI): a
  standalone GRUB EFI image (`make efi`) boots the kernel through GRUB's real
  Multiboot handoff and the kernel runs to "launching the desktop". This is a
  genuine bare-metal boot path, not QEMU's `-kernel`.
- **Framebuffer consumption + safe fallback.** `kmain` reads a Multiboot-
  provided linear framebuffer (`fb_init_mb`, `multiboot.h` framebuffer fields)
  when the bootloader supplies one; otherwise it falls back to the Bochs/std-VGA
  DISPI mode-set. So `make run` (QEMU `-kernel`, which does **not** provide a
  Multiboot framebuffer) is completely unaffected. The procedural desktop
  wallpaper (M1291) needs no image on disk, so the desktop looks right with no
  filesystem too.

## Build a bootable image

    make efi     # UEFI: build/BOOTX64.EFI  (no extra tools needed; verified to boot)
    make iso     # BIOS: build/os.iso       (needs xorriso: emerge dev-libs/libisoburn)

**UEFI** — copy `build/BOOTX64.EFI` to `/EFI/BOOT/BOOTX64.EFI` on a FAT-formatted
USB stick and boot a UEFI machine. Verify in QEMU:

    qemu-system-x86_64 -enable-kvm -m 256M \
      -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2-ovmf/OVMF_CODE.fd \
      -drive if=pflash,format=raw,file=OVMF_VARS.fd \
      -drive format=raw,file=fat:rw:<dir-with-EFI/BOOT/BOOTX64.EFI> -serial stdio

**BIOS** — `make iso` then `dd if=build/os.iso of=/dev/sdX bs=4M` (X = the USB
stick). Verify: `qemu-system-x86_64 -cdrom build/os.iso -serial stdio` (no
`-kernel` — this exercises the real GRUB→Multiboot path).

## Multiboot2 framebuffer handoff (M1293) — works; on-screen display needs real HW

GRUB does **not** hand a framebuffer to a Multiboot**1** kernel (the info `flags`
framebuffer bit stays clear, even with video modules). So `boot/boot.asm` now
also carries a **Multiboot2** header alongside the MB1 one, and `kmain` converts
the MB2 tag list into the MB1-format struct (`mb2_to_mb1`) — memory map +
framebuffer flow through the rest of boot unchanged. Booted via GRUB's
`multiboot2` command (`make efi` / `make iso`), **GRUB hands over a real linear
framebuffer and the kernel consumes it** — verified under QEMU + OVMF:

    [ ok ] Multiboot framebuffer 1280x960 32-bpp @ 0x80000000 -- bare-metal graphics path

The kernel maps that LFB, runs to the desktop, and paints into it with no fault.
QEMU `-kernel` (Multiboot1) is unchanged — it gets no MB2 framebuffer and uses the
Bochs fallback exactly as before (`make check` stays green).

**One unverified step remains:** under QEMU + OVMF the painted desktop does not
appear on the emulated display — QEMU keeps scanning out OVMF's own 1280x800
buffer rather than the 1280x960 GOP framebuffer GRUB set (a known QEMU/OVMF
GOP-emulation quirk). On real UEFI hardware the GOP `FrameBufferBase` persists as
the live scanout after `ExitBootServices`, so the desktop should display — but
that final step needs a physical machine (none here). Note: the framebuffer GRUB
provided had a tight pitch (`5120 == 1280*4`), so no `fb.c` stride change was
needed; a GPU whose mode has a padded pitch would require `fb.c` to honor the
framebuffer pitch separately from the width.

## Real-hardware notes (from research)

- **Input:** primary input is PS/2 (IRQ1) + a USB tablet/keyboard over UHCI.
  Modern USB-only laptops (xHCI) may not deliver input to the desktop yet
  (EHCI/xHCI drivers exist but HID is wired through the UHCI path). Best on an
  older machine with a PS/2 port or BIOS USB-legacy emulation.
- **Disk:** apps are embedded in the kernel; only data files (README, etc.) live
  on the FAT image — so first boot reaches the desktop with no data disk.
- **Firmware:** the Multiboot1 trampoline targets the BIOS/CSM path; the UEFI
  `make efi` route works because GRUB exits boot services before the handoff.
