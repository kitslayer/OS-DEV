/* atapi.h — ATAPI CD-ROM read driver (SCSI PACKET over ATA, PIO). See atapi.c.
 * Additive to ata.c (which skips ATAPI-signature devices); read-only, 2048-byte
 * logical sectors. (M1852) */
#ifndef ATAPI_H
#define ATAPI_H
#include <stdint.h>

#define ATAPI_MAX 4   /* the four legacy ATA slots (primary/secondary x master/slave) */

void atapi_init(void);        /* probe the ATA channels for ATAPI CD-ROM devices */
void atapi_selftest(void);    /* boot self-test: read + verify a CD's ISO 9660 PVD (no-op without a CD) */
int  atapi_present(int slot);
int  atapi_read_capacity(int slot, uint32_t *last_lba, uint32_t *block_size);   /* 0/-1 */
int  atapi_read10(int slot, uint32_t lba, uint16_t count, uint8_t *buf, int buflen);   /* bytes/-1 */

#endif /* ATAPI_H */
