# Milestone 12 — PCI bus enumeration

**Goal:** discover the hardware plugged into the machine. Real devices (the
network card, the GPU) sit on the PCI bus, and you can't drive one until you've
found it and learned where its registers live.

## PCI configuration space

Every PCI function exposes a 256-byte **configuration space** describing itself:
vendor & device IDs, a class code (what kind of device it is), the interrupt
line it uses, and **Base Address Registers (BARs)** — which tell you where the
device's memory-mapped registers or I/O ports are.

You reach it through two I/O ports (the legacy "configuration mechanism #1"):
write a packed `(bus, slot, function, offset)` address to **0xCF8**, then
read/write the 32-bit value at **0xCFC**. (`kernel/pci.c`)

## Enumeration

Brute force: probe every bus (0–255) × slot (0–31) × function (0–7). A vendor ID
of `0xFFFF` means "nothing here." For everything else we read the device/class.
`pci_find(vendor, device)` returns the first match; `pci_bar()`,
`pci_irq_line()`, and `pci_enable_bus_master()` pull out what a driver needs.

`pci_enable_bus_master` is important for the NIC: it sets the bus-master bit so
the device is allowed to **DMA** to/from RAM (read and write descriptor rings on
its own).

## What we found

```
00:00.0  8086:1237  class 06:00   Intel 440FX host bridge
00:01.1  8086:7010  class 01:01   PIIX3 IDE controller   (our disk)
00:02.0  1234:1111  class 03:00   QEMU VGA               (M14 framebuffer)
00:03.0  8086:100e  class 02:00   Intel e1000 NIC        (M13 networking)
```

Class `02` = network controller, `03` = display — exactly the two devices the
next milestones target.

## Files
- `kernel/pci.c`, `kernel/include/pci.h` — config access, find, BARs, enumerate
- `kernel/include/io.h` — gained `outl`/`inl` (32-bit port I/O)
