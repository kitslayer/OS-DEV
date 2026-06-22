/*
 * svga.h — VMware SVGA-II paravirtual 2D display driver.
 *
 * Where kernel/fb.c + kernel/bochs_vbe.c drive QEMU's std-VGA via the Bochs
 * DISPI register interface, this drives the OTHER widely-emulated paravirtual
 * 2D adapter: VMware SVGA-II (QEMU `-device vmware-svga` / `-vga vmware`,
 * PCI 0x15AD:0x0405). It is programmed very differently from DISPI:
 *
 *   - a pair of PCI I/O ports form an INDEX/VALUE register file (write a
 *     register number to the index port, then read/write its 32-bit value at
 *     the value port) — that's how you confirm the device id, set the mode,
 *     and read the framebuffer/FIFO addresses;
 *   - a LINEAR FRAMEBUFFER (BAR1) you write 0x00RRGGBB pixels into (the SAME
 *     pixel layout fb.c uses), but the host does NOT continuously scan it —
 *   - a COMMAND FIFO (BAR2, a ring of 32-bit words) into which you emit an
 *     SVGA_CMD_UPDATE rectangle and then poke SVGA_REG_SYNC, telling the host
 *     which region of the framebuffer changed so it presents it.
 *
 * So a "present" is: write pixels to the BAR1 framebuffer, push an UPDATE
 * command (x,y,w,h) into the FIFO ring, bump the FIFO's NEXT_CMD pointer, and
 * sync. svga_present() does exactly that.
 *
 * SAFE SCOPE: this is ADDITIVE, exactly like kernel/virtio_gpu.c. The boot
 * display stays on the linear framebuffer (fb.c / bochs_vbe.c) unchanged;
 * svga_init() probes PCI and is a clean no-op (returns -1) when no VMware
 * SVGA-II device is attached, so a machine without one boots byte-identically.
 * Like virtio_gpu_selftest()/hda_selftest() prove their pipeline headlessly,
 * svga_selftest() programs a mode, writes a colour-band test pattern to the
 * framebuffer, emits an UPDATE over the whole screen, and reads registers back
 * to confirm — the headless proof, with fb.c untouched.
 */
#pragma once
#include <stdint.h>

/* Probe the PCI bus for a VMware SVGA-II device (0x15AD:0x0405) and bring it
 * up: confirm SVGA_ID_2 via the index/value register file, read the linear
 * framebuffer + command FIFO base/size, program WIDTH/HEIGHT/BPP=32 + enable,
 * and initialize the FIFO ring. Returns 0 if a usable device was initialized,
 * -1 if none is present / init failed (a clean no-op — the linear-framebuffer
 * boot display path is untouched either way). */
int svga_init(void);

/* 1 if svga_init() brought a device up (mode set + FIFO live), else 0. */
int svga_active(void);

/* The mode set on the device (the framebuffer is width*height*4 bytes, indexed
 * as y*width + x). 0 if no device. */
int svga_width(void);
int svga_height(void);

/* The mapped BAR1 linear framebuffer: a width*height array of 0x00RRGGBB pixels
 * (the SAME pixel layout fb.c uses). NULL if no device. Write into this, then
 * call svga_present() to push the changed rectangle to the host. */
uint32_t *svga_framebuffer(void);

/* Present the rectangle [x,y,w,h] of the framebuffer: emit an SVGA_CMD_UPDATE
 * for that rect into the command FIFO, bump NEXT_CMD, and poke SVGA_REG_SYNC.
 * The rect is clamped within [0,width]x[0,height]. Returns 0 on success, -1 on
 * bad-arg / no device. This is the "flush" the desktop compositor would call
 * after drawing a frame. */
int svga_present(int x, int y, int w, int h);

/* Bring-up self-test (the headless proof, like virtio_gpu_selftest): fill the
 * framebuffer with a known colour-band test pattern, emit an UPDATE over the
 * whole screen, sync, and read registers back (SVGA_ID_2, FB/FIFO addresses,
 * mode) to confirm. No-op (logs "none attached") if no device. */
void svga_selftest(void);
