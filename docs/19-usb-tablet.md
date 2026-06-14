# Milestone 19 — USB tablet (absolute pointer)

**Goal:** make the cursor track the real mouse correctly. The PS/2 mouse is
*relative*, so in a window it can't match the host pointer (the "mouse doesn't
scale right" problem). A USB **tablet** reports *absolute* coordinates, so the
cursor maps 1:1 — but reading it means writing a USB host-controller driver.

## UHCI: USB over DMA (`kernel/usb.c`)

We drive **UHCI** (USB 1.1), the simplest controller — I/O ports plus DMA
structures in RAM:

- a **frame list** (1024 entries) the controller walks ~1000×/sec;
- each entry points at a **queue head (QH)**;
- a QH points at a chain of **transfer descriptors (TDs)**, each describing one
  packet (SETUP / IN / OUT) with a buffer.

We keep one QH for **control** transfers (enumeration) and one for the tablet's
**interrupt-IN** endpoint (the stream of HID reports). All of it lives in
identity-mapped low RAM so physical == virtual, which the controller's DMA needs.

## Enumerating the device

Standard USB bring-up over control transfers: reset the root port, `GET_DESCRIPTOR`
(device) to learn the endpoint-0 max packet size, `SET_ADDRESS` to give the
device address 1, `GET_DESCRIPTOR` (configuration) and walk it to find the
interrupt-IN endpoint, then `SET_CONFIGURATION`. After that we arm a TD on the
interrupt endpoint and poll it for reports.

## The two bugs worth remembering

1. **A 32-bit register written 16 bits wide.** The frame-list base (`FRBASE`) is
   32-bit; writing it with `outw` truncated the address, so the controller
   walked garbage and *no* descriptors executed (TDs stayed "active,
   untouched"). Fixed with `outl`.
2. **One TD per frame → timeout.** UHCI only advances to the next TD on the next
   1 ms frame unless you set the **depth-first (Vf) bit** on the TD links. A
   multi-packet transfer was taking several milliseconds while a fixed
   spin-count poll expired early (it failed *nondeterministically*, the tell-tale
   sign of a timing bug). Fixed by setting Vf and using a timer-based timeout.

## Reading the tablet

The tablet's HID report is `[buttons][x_lo][x_hi][y_lo][y_hi][wheel]`, where X/Y
are absolute in `0..32767`. We scale those to screen pixels and call
`mouse_set_abs()`. The desktop polls the endpoint every frame. We verified it by
injecting an absolute coordinate (top-right) over QMP and seeing the cursor
appear at exactly that pixel — no drift, no grab.

## Wiring
`kmain` prefers the tablet (`usb_tablet_init`); if there's no controller/tablet
it falls back to the relative PS/2 mouse. Either way the window manager reads
`mouse_x/y/buttons`, so nothing above the driver changed.

## Files
- `kernel/usb.c`, `kernel/include/usb.h` — UHCI + enumeration + tablet polling
- `kernel/mouse.c` — `mouse_set_abs` (absolute position)
- Makefile — adds `-device piix3-usb-uhci -device usb-tablet`
