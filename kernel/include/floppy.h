/*
 * floppy.h — 82077AA floppy disk controller (FDC) driver, via ISA DMA.
 *
 * The classic PC floppy controller is a legacy-I/O-port device (registers at
 * 0x3F0..0x3F7, IRQ6) much like the IDE controller kernel/ata.c drives. What
 * makes it *new* for this kernel is how it moves data: every other DMA driver
 * here (ahci/nvme/virtio_blk/e1000) is a PCI bus-master that fetches descriptors
 * from RAM itself, but the FDC has no bus-master engine — it streams its bytes
 * through the legacy 8237 DMA controller (ISA DMA, channel 2). ISA DMA needs a
 * bounce buffer in the low 16 MiB that does NOT cross a 64 KiB physical boundary;
 * we drive the 8237's mask/mode/address/page/count registers by hand to point it
 * at that buffer, then let the FDC and the 8237 cooperate to fill it.
 *
 * This is an *additional* block device, exactly like ahci.c / nvme.c: the OS
 * still BOOTS from and runs FAT32/VFS on the legacy ATA disk (kernel/ata.c).
 * floppy_init() resets + recalibrates the controller; if there is no FDC (its
 * status register reads all-ones / never goes ready) or no diskette, it is a
 * clean no-op that returns -1, so a machine without a floppy boots unchanged.
 *
 * Geometry is fixed to the standard 1.44 MB 3.5" diskette: 80 cylinders, 2 heads,
 * 18 sectors/track, 512 B/sector = 2880 sectors. We never write the medium (the
 * driver is read-only), matching the additive, verifiable shape of the task.
 */
#pragma once
#include <stdint.h>

#define FLOPPY_SECTOR_SIZE   512u
#define FLOPPY_SECTORS_TRACK 18u
#define FLOPPY_HEADS         2u
#define FLOPPY_CYLINDERS     80u
/* 80 * 2 * 18 = 2880 sectors on a 1.44 MB diskette. */
#define FLOPPY_TOTAL_SECTORS (FLOPPY_CYLINDERS * FLOPPY_HEADS * FLOPPY_SECTORS_TRACK)

/* Reset + recalibrate the floppy controller and spin the motor up. Returns 0 if
 * a usable 1.44 MB FDC + diskette came up, or -1 if no controller/diskette is
 * present or bring-up failed (a clean no-op — the legacy-ATA boot is unaffected,
 * and every wait inside has a finite timeout so an absent FDC never hangs). */
int floppy_init(void);

/* 1 if floppy_init() brought a controller + diskette up, else 0. */
int floppy_present(void);

/* Read `count` 512-byte sectors starting at LBA `lba` into `buf` (which must
 * hold count*512 bytes). The transfer goes through an internal ISA-DMA bounce
 * buffer and is memcpy()'d into `buf`, so `buf` has no alignment constraint.
 * `count` is capped at one track (18 sectors) per call and the read never
 * crosses a track boundary in a single FDC command (the CHS geometry forbids
 * it), so a caller asking for more is served by looping. lba+count must be
 * within the 2880-sector geometry. Returns 0 on success, -1 on bad-arg / absent
 * controller / seek or read error / timeout. */
int floppy_read(uint32_t lba, uint32_t count, void *buf);

/* Boot-time self-test: if a floppy + diskette is present, read sector 0 and a
 * couple more, log the first 16 bytes + an additive checksum per sector (so the
 * read can be matched byte-exact against known on-disk content), and confirm the
 * reset + recalibrate succeeded. No-op (logs "no floppy") if no FDC/diskette is
 * attached. Mirrors ahci_selftest / nvme_selftest. */
void floppy_selftest(void);
