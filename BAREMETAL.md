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
that final step needs a physical machine (none here). `fb.c` now honors the
framebuffer **pitch** separately from the width (`fb_stride`): a real GPU whose
GOP mode has a padded row stride (`pitch > width*4`) is mapped + presented
row-by-row at the reported pitch, so it isn't rejected. (Tried under QEMU+OVMF:
an explicit 1280×960 request gives a tight-pitch FB the kernel consumes but QEMU
doesn't scan out — mode mismatch; a "no-preference" request gives GRUB's native
mode but OVMF reports it in a form `fb_init_mb` declines — both are QEMU/OVMF
emulation quirks, not kernel faults.)

## Getting in over the network — the netcon debug console (M1870)

The framebuffer-display step above is the one thing that can't be fully verified
without a physical machine. So the bring-up plan doesn't depend on the screen at
all: the kernel can serve a **debug console over ethernet**. It's the safety net —
if the display stays dark, you still get a shell.

`kernel/netcon.c` is an in-kernel task that (when the kernel cmdline carries
`netcon`) brings the NIC up, takes a DHCP lease, and LISTENS on **TCP 2323**,
serving a line-oriented inspection shell. Because it runs in the kernel — not as a
ring-3 app, whose `print()` only reaches the framebuffer — it can dump the boot
log and kernel state and reboot the box, all remotely:

    help    dmesg   mem   ps    cpu   uptime
    net     ip      pci   bcache   ls [path]   cat <path>   echo <text>   reboot

### Build a bring-up image (cmdline already set to `netcon`)

    make efi-bringup   # UEFI: build/BOOTX64-bringup.EFI
    make iso-bringup   # BIOS: build/os-bringup.iso   (needs xorriso)

Deploy exactly like the normal images (`BOOTX64-bringup.EFI` → `/EFI/BOOT/
BOOTX64.EFI` on a FAT USB; or `dd` the ISO). These default to a GRUB menuentry
that boots `multiboot2 /boot/kernel32.elf netcon`.

### Connect

1. Boot the machine on a LAN with a DHCP server, NIC connected.
2. Find its address: look up the OS-DEV NIC's MAC in your router's DHCP-lease
   table (netcon logs the MAC + leased IP to serial/screen too, if either works).
3. `nc <ip> 2323`, then type `help`. (`echo hi` is a quick link check; `dmesg`
   shows the whole boot log; `pci` shows exactly what hardware the machine has.)

### Safe on a machine with real disks (`nodisk`)

The bring-up images set `nodisk` on the kernel cmdline alongside `netcon`. This is
important: OS-DEV's boot normally runs disk-**write** self-tests (ata-dma, ata-
cache, nvme, virtio-blk, the blockdev buffer-cache test, and the dm-RAID test at
LBA 64) against whatever writable disks it finds — and on real hardware "whatever
it finds" is the machine's actual internal disk. Even though each test saves and
restores the sector, a power loss mid-test would leave permanent corruption, and
the LBAs chosen (last sector = backup GPT header; LBA 64 = filesystem metadata)
are not actually safe. `nodisk` skips every disk-write self-test AND the boot
FAT32 mount, so OS-DEV touches **no disk at all** — verified by booting with a
scratch NVMe attached and confirming its contents are byte-identical afterward.
Boot without `nodisk` only on a machine whose disks you are willing to lose.

### Notes / limits

- **Networking needs a supported NIC.** netcon (and all OS-DEV networking) has
  drivers for the Intel **e1000**, the old Realtek **RTL8139**, and **virtio-net**
  only. A newer Realtek gigabit part (RTL8111/8168) or a recent Intel (i2xx) will
  NOT bind — netcon logs "no supported NIC found" and there's no network path.
  On a laptop with a working display this is fine (the screen is the channel);
  netcon is the fallback for a headless/dark-framebuffer box with a supported NIC.

- **Opt-in, and it replaces the boot net self-test.** The net stack has no
  cross-connection RX demux (every receiver polls `nic_receive` directly) and the
  NIC RX rings aren't safe for two concurrent pollers, so a netcon boot skips
  `net_demo`'s internet self-test and netcon owns the network. Don't run
  `wsserve`/on-demand `httpd` during a netcon session (they share one server
  connection slot). A normal (non-`netcon`) boot is completely unaffected.
- **Verified end-to-end under QEMU + OVMF** (the real GRUB→Multiboot2 path, not
  `-kernel`): the `netcon` cmdline reaches the kernel, netcon takes a DHCP lease,
  and a host TCP client drives a full multi-command session — booting once, no
  fault. Fixing this exposed a real latent bug: `mb2_to_mb1` wasn't copying the
  Multiboot2 command-line tag, so **no** cmdline flag (gdbstub, netcon, …) had
  ever reached a GRUB-booted kernel; now it does.

## Real-hardware notes (from research)

- **Input:** primary input is PS/2 (IRQ1) + a USB tablet/keyboard over UHCI.
  Modern USB-only laptops (xHCI) may not deliver input to the desktop yet
  (EHCI/xHCI drivers exist but HID is wired through the UHCI path). Best on an
  older machine with a PS/2 port or BIOS USB-legacy emulation.
- **Disk:** apps are embedded in the kernel; only data files (README, etc.) live
  on the FAT image — so first boot reaches the desktop with no data disk.
- **Firmware:** the Multiboot1 trampoline targets the BIOS/CSM path; the UEFI
  `make efi` route works because GRUB exits boot services before the handoff.
