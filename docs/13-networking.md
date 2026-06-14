# Milestone 13 — NIC driver + ARP + ping

**Goal:** get on the network. Drive a real network card, then speak just enough
of two protocols (ARP and ICMP) to resolve the gateway and ping it. This is the
first real step toward streaming anything from the NAS.

## The NIC: DMA, not port pokes (`kernel/e1000.c`)

A gigabit NIC is too fast to feed a byte at a time. The Intel e1000 uses
**DMA** through two **descriptor rings** in RAM:

- **RX ring:** an array of descriptors, each pointing at an empty buffer. The
  card writes incoming frames into them and sets a "descriptor done" (DD) bit.
- **TX ring:** we fill a descriptor with a packet buffer + length, bump the
  tail register, and the card DMAs it onto the wire and sets DD.

Setup: find the card on PCI (`8086:100e`), enable bus-mastering so it may DMA,
map its registers (BAR0) as **cache-disabled** MMIO, read the MAC from its
EEPROM, allocate the rings + buffers from the PMM (their physical addresses are
what the card's DMA engine uses), and point the ring registers at them. We mask
the card's interrupts and **poll** the DD bits — simplest for now.

## ARP: who has this IP? (`kernel/net.c`)

You can't send an IP packet to a host without its **MAC address**. ARP asks, on
the local link: "who has 10.0.2.2? tell 10.0.2.15." We broadcast the request and
the gateway replies with its MAC. (QEMU's user-mode networking emulates the
gateway at 10.0.2.2 and answers.)

## ICMP echo: ping

With the gateway's MAC known, we build a full **Ethernet → IPv4 → ICMP** echo
request, complete with the two **Internet checksums** (a one's-complement sum
over the IP header and over the ICMP message), send it, and poll for the echo
reply. We got 3/3 replies.

Packets are built **byte-by-byte in network (big-endian) order** rather than via
C structs, which sidesteps struct-packing and byte-order pitfalls.

## What we proved

```
our MAC = 52:54:00:12:34:56          (read from the card's EEPROM)
ARP: 10.0.2.2 is at 52:55:0a:00:02:02 (the gateway answered)
ping 10.0.2.2: reply seq=1/2/3        (round-trip ICMP works)
```

TX, RX, ARP, IPv4, and ICMP all function end to end.

## The road from here to NAS music

This is the bottom of the networking stack. To reach the goal we still need:
**UDP + TCP**, **DNS**, an **HTTP** (or SMB/NFS) client to fetch from the NAS,
then an **audio driver** (AC'97/Intel-HDA) and a decoder. But the hard part —
making the metal move packets — is done.

## Files
- `kernel/e1000.c` — the NIC driver (rings, MAC, send/receive)
- `kernel/net.c` — ARP + ICMP + the demo
- builds on `kernel/pci.c` (find the card) and the PMM/VMM (DMA memory + MMIO)
