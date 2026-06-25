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

## The remaining gap: a framebuffer from GRUB (graphics on real hardware)

The kernel is *ready* to use a Multiboot framebuffer, but GRUB does not yet hand
one over for our Multiboot**1** kernel (the info `flags` framebuffer bit stays
clear, even with video modules + `gfxpayload`). Result: under real GRUB the
kernel boots and runs but has no linear framebuffer, so the graphical desktop
does not paint (it reaches the desktop logic but the screen stays on the
firmware logo). This is the well-known #1 gap for QEMU-only hobby OSes.

**Next step:** switch the boot path to **Multiboot2**, whose framebuffer *tag*
is the robust, well-supported way GRUB reports the LFB (base/pitch/width/height/
bpp). That means a Multiboot2 header in `boot/boot.asm` + a small Multiboot2
info parser (tag walk) feeding `fb_init_mb`. `fb.c` indexes the LFB as
`y*width + x`, so also store/honor the framebuffer **pitch** if GRUB's mode has
padding (`pitch != width*4`).

## Real-hardware notes (from research)

- **Input:** primary input is PS/2 (IRQ1) + a USB tablet/keyboard over UHCI.
  Modern USB-only laptops (xHCI) may not deliver input to the desktop yet
  (EHCI/xHCI drivers exist but HID is wired through the UHCI path). Best on an
  older machine with a PS/2 port or BIOS USB-legacy emulation.
- **Disk:** apps are embedded in the kernel; only data files (README, etc.) live
  on the FAT image — so first boot reaches the desktop with no data disk.
- **Firmware:** the Multiboot1 trampoline targets the BIOS/CSM path; the UEFI
  `make efi` route works because GRUB exits boot services before the handoff.
