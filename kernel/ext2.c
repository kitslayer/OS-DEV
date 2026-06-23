/*
 * ext2.c — read-only ext2. See ext2.h. Superblock @ byte 1024; block-group
 * descriptors after it; an inode's 15 i_block pointers give direct (0-11),
 * single-indirect (12) and double-indirect (13) data blocks; directories are a
 * chain of {inode, rec_len, name_len, file_type, name} records. Root = inode 2.
 * Standard layouts (inode_size 128/256, block_size 1024-4096) only.
 */
#include "ext2.h"
#include <stdint.h>

#define EXT2_MAGIC    0xEF53
#define EXT2_ROOT_INO 2
#define SECSZ         512

static uint16_t e_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t e_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    blk_read_fn read; void *ctx; uint64_t start;
    uint32_t block_size, inodes_per_group, inode_size, gdt_block;
} ext2_t;

/* read ext2 block `blk` (block_size bytes) into buf (>= block_size) */
static int rdblk(ext2_t *v, uint32_t blk, uint8_t *buf) {
    uint32_t spb = v->block_size / SECSZ;
    return v->read(v->ctx, v->start + (uint64_t)blk * spb, spb, buf);
}

/* parse + validate the superblock; 0 on a supported ext2, -1 otherwise */
static int ext2_open(blk_read_fn read, void *ctx, uint64_t start, ext2_t *v) {
    uint8_t sb[1024];
    if (read(ctx, start + 2, 2, sb) < 0) return -1;        /* superblock: byte 1024 = LBA+2, 1024 bytes */
    if (e_rd16(sb + 56) != EXT2_MAGIC) return -1;
    uint32_t logbs = e_rd32(sb + 24);
    if (logbs > 2) return -1;                              /* only 1024/2048/4096 */
    v->read = read; v->ctx = ctx; v->start = start;
    v->block_size = 1024u << logbs;
    v->inodes_per_group = e_rd32(sb + 40);
    uint32_t rev = e_rd32(sb + 76);
    v->inode_size = (rev >= 1) ? e_rd16(sb + 88) : 128;
    if (!v->inodes_per_group || v->inode_size < 128 || v->inode_size > 256) return -1;
    v->gdt_block = (v->block_size == 1024) ? 2 : 1;        /* GDT follows the superblock's block */
    return 0;
}

/* read inode `ino` (1-based) into out (>= inode_size bytes); 0/-1 */
static int read_inode(ext2_t *v, uint32_t ino, uint8_t *out) {
    if (ino == 0) return -1;
    uint32_t group = (ino - 1) / v->inodes_per_group;
    uint32_t index = (ino - 1) % v->inodes_per_group;
    uint8_t gd[4096];
    uint32_t gd_per_block = v->block_size / 32;
    if (rdblk(v, v->gdt_block + group / gd_per_block, gd) < 0) return -1;
    uint32_t inode_table = e_rd32(gd + (group % gd_per_block) * 32 + 8);
    uint64_t byte_off = (uint64_t)index * v->inode_size;
    uint8_t b[4096];
    if (rdblk(v, inode_table + (uint32_t)(byte_off / v->block_size), b) < 0) return -1;
    uint32_t off = (uint32_t)(byte_off % v->block_size);
    if (off + v->inode_size > v->block_size) return -1;    /* inode straddles a block (non-standard) */
    for (uint32_t i = 0; i < v->inode_size; i++) out[i] = b[off + i];
    return 0;
}

/* map a file-relative block index to its disk block (0 = sparse hole) */
static uint32_t map_block(ext2_t *v, const uint8_t *inode, uint32_t fblk) {
    const uint8_t *ib = inode + 40;                        /* i_block[15] */
    uint32_t ppb = v->block_size / 4;
    if (fblk < 12) return e_rd32(ib + fblk * 4);           /* direct */
    fblk -= 12;
    uint8_t buf[4096];
    if (fblk < ppb) {                                      /* single-indirect */
        uint32_t ind = e_rd32(ib + 12 * 4);
        if (!ind || rdblk(v, ind, buf) < 0) return 0;
        return e_rd32(buf + fblk * 4);
    }
    fblk -= ppb;
    if (fblk < ppb * ppb) {                                /* double-indirect */
        uint32_t dind = e_rd32(ib + 13 * 4);
        if (!dind || rdblk(v, dind, buf) < 0) return 0;
        uint32_t ind = e_rd32(buf + (fblk / ppb) * 4);
        if (!ind || rdblk(v, ind, buf) < 0) return 0;
        return e_rd32(buf + (fblk % ppb) * 4);
    }
    return 0;                                              /* triple-indirect unsupported */
}

/* find `name` in directory inode `dino`; returns the child inode #, 0 if absent */
static uint32_t dir_lookup(ext2_t *v, const uint8_t *dino, const char *name, int *is_dir) {
    uint32_t size = e_rd32(dino + 4);
    uint8_t blk[4096];
    for (uint32_t off = 0; off < size; off += v->block_size) {
        uint32_t db = map_block(v, dino, off / v->block_size);
        if (!db || rdblk(v, db, blk) < 0) continue;
        uint32_t bo = 0;
        while (bo + 8 <= v->block_size) {
            uint32_t ino = e_rd32(blk + bo);
            uint16_t rl = e_rd16(blk + bo + 4);
            uint8_t nl = blk[bo + 6];
            if (rl < 8) break;
            if (ino && nl && bo + 8 + nl <= v->block_size) {
                int match = 1;
                for (int k = 0; k < nl; k++) if (name[k] == 0 || (uint8_t)name[k] != blk[bo + 8 + k]) { match = 0; break; }
                if (match && name[nl] == 0) { if (is_dir) *is_dir = (blk[bo + 7] == 2); return ino; }
            }
            bo += rl;
        }
    }
    return 0;
}

/* walk a '/'-path from root; fills inode_out (>=256B) + *is_dir, returns the inode # or 0 */
static uint32_t walk(ext2_t *v, const char *path, uint8_t *inode_out, int *is_dir) {
    if (read_inode(v, EXT2_ROOT_INO, inode_out) < 0) return 0;
    if (is_dir) *is_dir = 1;
    uint32_t ino = EXT2_ROOT_INO;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char comp[256]; int n = 0;
        while (*p && *p != '/' && n < 255) comp[n++] = *p++;
        comp[n] = 0;
        int cd = 0;
        uint32_t child = dir_lookup(v, inode_out, comp, &cd);
        if (!child || read_inode(v, child, inode_out) < 0) return 0;
        ino = child;
        if (is_dir) *is_dir = cd;
    }
    return ino;
}

int ext2_probe(blk_read_fn read, void *ctx, uint64_t start_lba) {
    ext2_t v;
    return ext2_open(read, ctx, start_lba, &v);
}

long ext2_read_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path,
                    void *buf, unsigned long max) {
    ext2_t v; if (ext2_open(read, ctx, start_lba, &v) < 0) return -1;
    uint8_t inode[256]; int isdir = 0;
    if (!walk(&v, path, inode, &isdir) || isdir) return -1;
    uint32_t size = e_rd32(inode + 4);
    if (size > max) size = (uint32_t)max;
    uint8_t blk[4096]; uint32_t done = 0;
    while (done < size) {
        uint32_t db = map_block(&v, inode, done / v.block_size);
        uint32_t chunk = v.block_size; if (chunk > size - done) chunk = size - done;
        if (!db) { for (uint32_t i = 0; i < chunk; i++) ((uint8_t *)buf)[done + i] = 0; }   /* hole */
        else { if (rdblk(&v, db, blk) < 0) break; for (uint32_t i = 0; i < chunk; i++) ((uint8_t *)buf)[done + i] = blk[i]; }
        done += chunk;
    }
    return (long)done;
}

int ext2_isdir_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path) {
    ext2_t v; if (ext2_open(read, ctx, start_lba, &v) < 0) return -1;
    uint8_t inode[256]; int isdir = 0;
    if (!walk(&v, path, inode, &isdir)) return -1;
    return isdir;
}

int ext2_list_path(blk_read_fn read, void *ctx, uint64_t start_lba, const char *path,
                   fatvol_dirent *out, int max) {
    ext2_t v; if (ext2_open(read, ctx, start_lba, &v) < 0) return -1;
    uint8_t inode[256]; int isdir = 0;
    if (!walk(&v, path, inode, &isdir) || !isdir) return -1;
    uint32_t size = e_rd32(inode + 4);
    uint8_t blk[4096]; int n = 0;
    for (uint32_t off = 0; off < size && n < max; off += v.block_size) {
        uint32_t db = map_block(&v, inode, off / v.block_size);
        if (!db || rdblk(&v, db, blk) < 0) continue;
        uint32_t bo = 0;
        while (bo + 8 <= v.block_size && n < max) {
            uint32_t ino = e_rd32(blk + bo);
            uint16_t rl = e_rd16(blk + bo + 4);
            uint8_t nl = blk[bo + 6];
            if (rl < 8) break;
            if (ino && nl && bo + 8 + nl <= v.block_size) {
                /* skip "." and ".." */
                int dot = (nl == 1 && blk[bo + 8] == '.') || (nl == 2 && blk[bo + 8] == '.' && blk[bo + 9] == '.');
                if (!dot) {
                    int k = 0; for (; k < nl && k < 12; k++) out[n].name[k] = (char)blk[bo + 8 + k];
                    out[n].name[k] = 0;
                    out[n].is_dir = (blk[bo + 7] == 2);
                    uint8_t cino[256];                         /* child inode -> size */
                    out[n].size = (read_inode(&v, ino, cino) == 0 && !out[n].is_dir) ? e_rd32(cino + 4) : 0;
                    n++;
                }
            }
            bo += rl;
        }
    }
    return n;
}
