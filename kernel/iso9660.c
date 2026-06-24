/*
 * iso9660.c — read-only ISO 9660. See iso9660.h.
 *
 * On-disk shape (the bits we parse):
 *   - Logical sector = 2048 bytes (read as 4 contiguous 512-byte device sectors).
 *   - Volume Descriptor set starts at logical sector 16; each descriptor is one
 *     sector with type @0 and "CD001" @1. Type 1 = Primary Volume Descriptor.
 *   - The PVD holds the 34-byte root Directory Record at offset 156.
 *   - A Directory Record: len_dr @0; extent LBA @2 (both-endian, LE half first);
 *     data length @10 (both-endian); file flags @25 (bit1 = directory); file-id
 *     length @32; file id @33. id 0x00 = ".", 0x01 = "..". Records never cross a
 *     logical-sector boundary; a len_dr of 0 means "skip to the next sector".
 * Everything is treated as untrusted and bounds-checked. Read-only.
 */
#include "iso9660.h"

#define ISO_SECSZ      2048u
#define DEV_SECSZ      512u
#define ISO_SEC_LBAS   (ISO_SECSZ / DEV_SECSZ)   /* 4 device sectors per logical sector */
#define ISO_VD_START   16u                        /* first volume descriptor's logical sector */
#define ISO_VD_SCAN    16                         /* how many descriptors to scan for the PVD */

/* both-endian numeric fields store the little-endian copy first, so just read that */
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct { blk_read_fn read; void *ctx; uint64_t start; } iso_t;

/* read one 2048-byte logical sector `sec` into buf (>= 2048 bytes) */
static int rd_sec(iso_t *v, uint32_t sec, uint8_t *buf) {
    return v->read(v->ctx, v->start + (uint64_t)sec * ISO_SEC_LBAS, ISO_SEC_LBAS, buf);
}

/* Locate the Primary Volume Descriptor; return its root dir's extent + length. */
static int find_pvd(iso_t *v, uint32_t *root_lba, uint32_t *root_len) {
    uint8_t sec[ISO_SECSZ];
    for (int i = 0; i < ISO_VD_SCAN; i++) {
        if (rd_sec(v, ISO_VD_START + (uint32_t)i, sec) != 0) return -1;
        if (!(sec[1] == 'C' && sec[2] == 'D' && sec[3] == '0' && sec[4] == '0' && sec[5] == '1'))
            return -1;                         /* not ISO 9660 at all */
        if (sec[0] == 1) {                     /* Primary Volume Descriptor */
            const uint8_t *rr = sec + 156;     /* root directory record */
            *root_lba = rd32le(rr + 2);
            *root_len = rd32le(rr + 10);
            return (*root_len && *root_lba) ? 0 : -1;
        }
        if (sec[0] == 255) return -1;          /* descriptor-set terminator: no PVD */
    }
    return -1;
}

/* Compare a requested component `want` (NUL-terminated) to an ISO file id of
 * `idlen` bytes, case-insensitively, stopping at a ';' version suffix or a bare
 * trailing '.'. Returns 1 on a full match. */
static int name_eq(const char *want, const uint8_t *id, int idlen) {
    int j = 0;
    for (int i = 0; i < idlen; i++) {
        uint8_t c = id[i];
        if (c == ';') break;                   /* ";1" version suffix */
        if (c == '.' && (i + 1 >= idlen || id[i + 1] == ';')) break;  /* trailing dot */
        char a = want[j];
        if (a == 0) return 0;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (a != (char)c) return 0;
        j++;
    }
    return want[j] == 0;
}

/* Copy an ISO file id (stripping ";1"/trailing dot) into out[<=max], NUL-term. */
static void name_copy(const uint8_t *id, int idlen, char *out, int max) {
    int j = 0;
    for (int i = 0; i < idlen && j < max - 1; i++) {
        uint8_t c = id[i];
        if (c == ';') break;
        if (c == '.' && (i + 1 >= idlen || id[i + 1] == ';')) break;
        out[j++] = (char)c;
    }
    out[j] = 0;
}

/* Is this record the "." (0x00) or ".." (0x01) self/parent entry? */
static int is_special(const uint8_t *rec) {
    return rec[32] == 1 && (rec[33] == 0 || rec[33] == 1);
}

/* Find `name` among the directory whose extent is [dlba, dlba+dlen). */
static int find_in_dir(iso_t *v, uint32_t dlba, uint32_t dlen, const char *name,
                       uint32_t *olba, uint32_t *olen, int *odir) {
    uint8_t sec[ISO_SECSZ];
    uint32_t nsec = (dlen + ISO_SECSZ - 1) / ISO_SECSZ;
    for (uint32_t si = 0; si < nsec; si++) {
        if (rd_sec(v, dlba + si, sec) != 0) return -1;
        uint32_t off = 0;
        while (off + 33 <= ISO_SECSZ) {
            uint8_t lendr = sec[off];
            if (lendr == 0) break;                         /* rest of sector is padding */
            if (lendr < 34 || off + lendr > ISO_SECSZ) break;   /* malformed */
            const uint8_t *rec = sec + off;
            uint8_t fil = rec[32];
            if (off + 33 + fil <= ISO_SECSZ && !is_special(rec) && name_eq(name, rec + 33, fil)) {
                *olba = rd32le(rec + 2);
                *olen = rd32le(rec + 10);
                *odir = (rec[25] & 0x02) ? 1 : 0;
                return 0;
            }
            off += lendr;
        }
    }
    return -1;
}

/* Resolve `path` (relative to the volume root; "" or "/" = root) to an entry. */
static int resolve(iso_t *v, const char *path, uint32_t *olba, uint32_t *olen, int *odir) {
    uint32_t clba, clen;
    if (find_pvd(v, &clba, &clen) != 0) return -1;
    int cdir = 1;
    const char *p = path ? path : "";
    while (*p == '/') p++;
    while (*p) {
        char comp[64]; int n = 0;
        while (*p && *p != '/' && n < 63) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;
        if (n == 0) continue;
        if (!cdir) return -1;                  /* a path component under a non-directory */
        uint32_t nlba, nlen; int ndir;
        if (find_in_dir(v, clba, clen, comp, &nlba, &nlen, &ndir) != 0) return -1;
        clba = nlba; clen = nlen; cdir = ndir;
    }
    *olba = clba; *olen = clen; *odir = cdir;
    return 0;
}

int iso9660_probe(blk_read_fn read, void *ctx, uint64_t start_lba) {
    iso_t v = { read, ctx, start_lba };
    uint32_t rl, rs;
    return find_pvd(&v, &rl, &rs) == 0 ? 0 : -1;
}

int iso9660_isdir_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path) {
    iso_t v = { read, ctx, start_lba };
    uint32_t lba, len; int dir;
    if (resolve(&v, path, &lba, &len, &dir) != 0) return -1;
    return dir ? 1 : 0;
}

int iso9660_list_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path,
                      fatvol_dirent *out, int max) {
    iso_t v = { read, ctx, start_lba };
    uint32_t dlba, dlen; int dir;
    if (resolve(&v, path, &dlba, &dlen, &dir) != 0 || !dir) return -1;
    int cnt = 0;
    uint8_t sec[ISO_SECSZ];
    uint32_t nsec = (dlen + ISO_SECSZ - 1) / ISO_SECSZ;
    for (uint32_t si = 0; si < nsec && cnt < max; si++) {
        if (rd_sec(&v, dlba + si, sec) != 0) return cnt ? cnt : -1;
        uint32_t off = 0;
        while (off + 33 <= ISO_SECSZ && cnt < max) {
            uint8_t lendr = sec[off];
            if (lendr == 0) break;
            if (lendr < 34 || off + lendr > ISO_SECSZ) break;
            const uint8_t *rec = sec + off;
            uint8_t fil = rec[32];
            if (off + 33 + fil <= ISO_SECSZ && !is_special(rec)) {
                fatvol_dirent *d = &out[cnt++];
                name_copy(rec + 33, fil, d->name, (int)sizeof d->name);
                d->is_dir = (rec[25] & 0x02) ? 1 : 0;
                d->size = d->is_dir ? 0 : rd32le(rec + 10);
            }
            off += lendr;
        }
    }
    return cnt;
}

long iso9660_read_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path,
                       void *buf, unsigned long max) {
    iso_t v = { read, ctx, start_lba };
    uint32_t flba, flen; int dir;
    if (resolve(&v, path, &flba, &flen, &dir) != 0 || dir) return -1;
    uint32_t want = flen;
    if ((unsigned long)want > max) want = (uint32_t)max;
    uint8_t sec[ISO_SECSZ];
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0, si = 0;
    while (done < want) {
        if (rd_sec(&v, flba + si, sec) != 0) return -1;
        uint32_t chunk = want - done;
        if (chunk > ISO_SECSZ) chunk = ISO_SECSZ;
        for (uint32_t k = 0; k < chunk; k++) dst[done + k] = sec[k];
        done += chunk; si++;
    }
    return (long)want;
}
