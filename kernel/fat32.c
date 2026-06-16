/*
 * fat32.c — a small read/write FAT32 filesystem driver (with subdirectories).
 *
 * FAT32's layout, from the boot sector (BPB) outward:
 *
 *   [ reserved (incl. boot sector) ][ FAT #1 ][ FAT #2 ][ data clusters... ]
 *
 * The "File Allocation Table" is the heart of it: a big array indexed by
 * cluster number. FAT[c] holds the *next* cluster of a file, or an
 * end-of-chain marker. So a file is a linked list of clusters threaded through
 * the FAT. Directories are just files whose contents are 32-byte entries.
 *
 * We read the disk through the ATA driver and expose list/read to the VFS.
 */
#include "fat32.h"
#include "vfs.h"
#include "ata.h"
#include <stdint.h>

#define SECSZ 512
#define EOC   0x0FFFFFF8      /* >= this in a FAT entry means end-of-chain */

static uint32_t bytes_per_sec;
static uint32_t sec_per_clus;
static uint32_t fat_start;     /* first FAT sector */
static uint32_t data_start;    /* sector of cluster 2 */
static uint32_t root_cluster;
static uint32_t cwd_cluster;   /* current directory (for relative paths) */
static uint32_t num_fats;
static uint32_t fat_sectors;   /* sectors per FAT */
static uint32_t total_clusters; /* count of data clusters on the volume */

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t cluster_to_sector(uint32_t cl) {
    return data_start + (cl - 2) * sec_per_clus;
}

/* Look up the next cluster in a chain. */
static uint32_t fat_next(uint32_t cl) {
    uint8_t sec[SECSZ];
    uint32_t fat_offset = cl * 4;
    uint32_t fat_sec = fat_start + fat_offset / SECSZ;
    if (ata_read(fat_sec, 1, sec) < 0)
        return EOC;
    return rd32(sec + (fat_offset % SECSZ)) & 0x0FFFFFFF;
}

/* Follow a chain one step, but bail (return EOC) if we've taken more steps than
 * there are clusters — a corrupt/cyclic FAT can't legitimately chain that long,
 * so this stops an infinite loop from hanging the kernel inside a syscall. */
static uint32_t fat_step(uint32_t cl, uint32_t *steps) {
    if (total_clusters && ++(*steps) > total_clusters + 2) return EOC;
    return fat_next(cl);
}

/* A legitimate data cluster is in [2, total_clusters+2). A corrupt/cyclic FAT can
 * aim a chain at an out-of-range cluster that's still < EOC; treating that as
 * end-of-chain stops a wrong/garbage read (the LBA would wrap to 28 bits or the
 * drive would error). The read path is memory-safe regardless (every read targets
 * the fixed-size sector buffer), so this is robustness/correctness, not a memory
 * fix — and it also rejects cluster 0/1 (so an empty file's first-cluster-0 can't
 * underflow cluster_to_sector). total_clusters==0 (pre-mount) falls back to the
 * old cl<EOC test. */
static int cluster_in_range(uint32_t cl) {
    return cl >= 2 && cl < EOC && (!total_clusters || cl < total_clusters + 2);
}

/* Turn an 8.3 directory name (11 bytes, space-padded) into "NAME.EXT". */
static void format_83(const uint8_t *raw, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++)
        out[n++] = raw[i];
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++)
            out[n++] = raw[i];
    }
    out[n] = '\0';
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (up(*a) != up(*b)) return 0;
        a++; b++;
    }
    return up(*a) == up(*b);
}

/*
 * Walk a directory's cluster chain (starting at cluster `cl`), calling visit()
 * per valid entry. visit returns nonzero to stop early. Returns count visited.
 * A directory is just a file whose data is 32-byte entries, so this works for
 * the root and any subdirectory alike.
 */
typedef int (*dir_visit_fn)(const uint8_t *entry, const char *name, void *ctx);

static int walk_dir(uint32_t cl, dir_visit_fn visit, void *ctx) {
    uint8_t sec[SECSZ];
    int count = 0;
    uint32_t steps = 0;

    while (cluster_in_range(cl)) {
        uint32_t first = cluster_to_sector(cl);
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (ata_read(first + s, 1, sec) < 0)
                return count;
            for (int off = 0; off < SECSZ; off += 32) {
                uint8_t *e = sec + off;
                if (e[0] == 0x00)            /* end of directory */
                    return count;
                if (e[0] == 0xE5)            /* deleted */
                    continue;
                if (e[11] == 0x0F)           /* long-file-name entry */
                    continue;
                if (e[11] & 0x08)            /* volume label */
                    continue;
                char name[16];
                format_83(e, name);
                count++;
                if (visit(e, name, ctx))
                    return count;
            }
        }
        cl = fat_step(cl, &steps);
    }
    return count;
}

/* Find an entry by name within directory cluster `cl`. Returns 1 + its first
 * cluster / dir-flag / size, or 0 if not present. */
struct dfind { const char *want; uint32_t fc, size; int isdir, found; };
static int dfind_visit(const uint8_t *e, const char *name, void *ctx) {
    struct dfind *c = ctx;
    if (ieq(name, c->want)) {
        c->fc = (uint32_t)rd16(e + 26) | ((uint32_t)rd16(e + 20) << 16);
        c->size = rd32(e + 28);
        c->isdir = (e[11] & 0x10) ? 1 : 0;
        c->found = 1;
        return 1;
    }
    return 0;
}
static int dir_find(uint32_t cl, const char *name, uint32_t *fc, int *isdir, uint32_t *size) {
    struct dfind c = { name, 0, 0, 0, 0 };
    walk_dir(cl, dfind_visit, &c);
    if (!c.found) return 0;
    if (fc) *fc = c.fc; if (isdir) *isdir = c.isdir; if (size) *size = c.size;
    return 1;
}

/* Split `path` into its containing-directory cluster (*dir) and final
 * component (*leaf), descending subdirectories. Absolute (/foo) starts at the
 * root, relative at the cwd; "." and ".." are honoured. 0 on success. */
static int resolve(const char *path, uint32_t *dir, const char **leaf) {
    uint32_t cur = (path[0] == '/') ? root_cluster : cwd_cluster;
    const char *p = path;
    if (*p == '/') p++;
    for (;;) {
        const char *slash = p; while (*slash && *slash != '/') slash++;
        if (*slash == 0) { *dir = cur; *leaf = p; return 0; }   /* last = leaf */
        char comp[16]; int n = 0;
        for (const char *q = p; q < slash && n < 15; q++) comp[n++] = *q;
        comp[n] = 0;
        if (comp[0] == 0 || ieq(comp, ".")) { p = slash + 1; continue; }
        uint32_t fc; int isdir;
        if (ieq(comp, "..")) {                       /* up one (no-op at root) */
            if (dir_find(cur, "..", &fc, &isdir, 0)) cur = fc ? fc : root_cluster;
        } else {
            if (!dir_find(cur, comp, &fc, &isdir, 0) || !isdir) return -1;
            cur = fc;
        }
        p = slash + 1;
    }
}

static uint32_t alloc_cluster(void);              /* fwd: add_entry grows a full dir chain */
static void     fat_set(uint32_t cl, uint32_t val);

/* Append a directory entry (name83/attr/first-cluster/size) into directory
 * cluster `dircl`, reusing the first free or deleted slot, growing the dir's
 * cluster chain if it is completely full. 0 on success. */
static int add_entry(uint32_t dircl, const uint8_t name83[11], uint8_t attr,
                     uint32_t first, uint32_t size) {
    uint8_t sec[SECSZ];
    uint32_t cl = dircl, steps = 0, last = dircl;
    while (cl < EOC) {
        uint32_t firsts = cluster_to_sector(cl);
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (ata_read(firsts + s, 1, sec) < 0) return -1;
            for (int off = 0; off < SECSZ; off += 32) {
                uint8_t *e = sec + off;
                if (e[0] == 0x00 || e[0] == 0xE5) {
                    for (int i = 0; i < 32; i++) e[i] = 0;
                    for (int i = 0; i < 11; i++) e[i] = name83[i];
                    e[11] = attr;
                    e[20] = first >> 16; e[21] = first >> 24;
                    e[26] = first;       e[27] = first >> 8;
                    e[28] = size; e[29] = size >> 8; e[30] = size >> 16; e[31] = size >> 24;
                    ata_write(firsts + s, 1, sec);
                    return 0;
                }
            }
        }
        last = cl;
        cl = fat_step(cl, &steps);
    }
    /* The directory is full. A FAT32 directory is an ordinary cluster chain
     * (unlike FAT12/16's fixed-size root), so grow it: allocate a fresh cluster,
     * zero every 32-byte slot (so they read as free) and place this entry in the
     * first slot, THEN link it onto the chain — writing the cluster before
     * linking means a concurrent chain-walk never sees an uninitialised cluster.
     * Mirrors fat32_write's data-chain growth. */
    uint32_t newcl = alloc_cluster();
    if (newcl == 0) return -1;                       /* disk full */
    uint32_t firsts = cluster_to_sector(newcl);
    uint8_t z[SECSZ];
    for (int i = 0; i < SECSZ; i++) z[i] = 0;
    for (uint32_t s = 1; s < sec_per_clus; s++) ata_write(firsts + s, 1, z);   /* zero the rest of the cluster */
    for (int i = 0; i < 11; i++) z[i] = name83[i];   /* the entry, in slot 0 (the rest of z stays zero = free slots) */
    z[11] = attr;
    z[20] = first >> 16; z[21] = first >> 24;
    z[26] = first;       z[27] = first >> 8;
    z[28] = size; z[29] = size >> 8; z[30] = size >> 16; z[31] = size >> 24;
    ata_write(firsts, 1, z);
    fat_set(last, newcl);                            /* link the new cluster onto the dir chain */
    return 0;
}

/* ---- VFS: list ---- */

struct list_ctx { vfs_dirent *out; int max; int n; };

static int list_visit(const uint8_t *e, const char *name, void *ctx) {
    struct list_ctx *c = ctx;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
        return 0;                         /* hide "." and ".." */
    if (c->n >= c->max)
        return 1;
    int i = 0;
    while (name[i] && i < 62) { c->out[c->n].name[i] = name[i]; i++; }
    if (e[11] & 0x10) c->out[c->n].name[i++] = '/';   /* mark directories */
    c->out[c->n].name[i] = '\0';
    c->out[c->n].size = rd32(e + 28);
    c->n++;
    return 0;
}

static int fat32_list(vfs_dirent *out, int max) {
    struct list_ctx ctx = { out, max, 0 };
    walk_dir(cwd_cluster, list_visit, &ctx);          /* list the current dir */
    return ctx.n;
}

/* ---- VFS: read ---- */

static long fat32_read(const char *name, void *buf, unsigned long max) {
    uint32_t dir; const char *leaf;
    if (resolve(name, &dir, &leaf) < 0) return -1;
    uint32_t cl0, size; int isdir;
    if (!dir_find(dir, leaf, &cl0, &isdir, &size) || isdir) return -1;

    uint8_t sec[SECSZ];
    uint8_t *dst = buf;
    uint32_t remaining = size;
    unsigned long written = 0;
    uint32_t cl = cl0, steps = 0;

    while (cluster_in_range(cl) && remaining > 0) {
        uint32_t first = cluster_to_sector(cl);
        for (uint32_t s = 0; s < sec_per_clus && remaining > 0; s++) {
            if (ata_read(first + s, 1, sec) < 0)
                return (long)written;
            uint32_t chunk = remaining < SECSZ ? remaining : SECSZ;
            for (uint32_t i = 0; i < chunk && written < max; i++)
                dst[written++] = sec[i];
            remaining -= chunk;
        }
        cl = fat_step(cl, &steps);
    }
    return (long)written;
}

/* ---- write support ---- */

/* Write a FAT entry (in all FAT copies), preserving the reserved top bits. */
static void fat_set(uint32_t cl, uint32_t val) {
    uint8_t sec[SECSZ];
    uint32_t off = cl * 4;
    uint32_t rel = fat_start + off / SECSZ, fo = off % SECSZ;
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t s = rel + f * fat_sectors;
        if (ata_read(s, 1, sec) < 0) return;
        uint32_t nv = (rd32(sec + fo) & 0xF0000000u) | (val & 0x0FFFFFFFu);
        sec[fo] = nv; sec[fo+1] = nv>>8; sec[fo+2] = nv>>16; sec[fo+3] = nv>>24;
        ata_write(s, 1, sec);
    }
}

/* Find a free cluster, mark it end-of-chain, and return it (0 = disk full). */
static uint32_t alloc_cluster(void) {
    uint8_t sec[SECSZ];
    uint32_t per = SECSZ / 4;
    for (uint32_t s = 0; s < fat_sectors; s++) {
        if (ata_read(fat_start + s, 1, sec) < 0) return 0;
        for (uint32_t e = 0; e < per; e++) {
            uint32_t cl = s * per + e;
            if (cl < 2) continue;
            if ((rd32(sec + e * 4) & 0x0FFFFFFF) == 0) {
                fat_set(cl, EOC);
                return cl;
            }
        }
    }
    return 0;
}

static void write_cluster(uint32_t cl, const uint8_t *data, uint32_t len) {
    uint32_t first = cluster_to_sector(cl);
    for (uint32_t s = 0; s < sec_per_clus; s++) {
        uint8_t sec[SECSZ];
        for (int i = 0; i < SECSZ; i++) sec[i] = 0;
        uint32_t chunk = len > SECSZ ? SECSZ : len;
        for (uint32_t i = 0; i < chunk; i++) sec[i] = data[i];
        ata_write(first + s, 1, sec);
        data += chunk; len -= chunk;
    }
}

static char up_c(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static void to_83(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, o = 0;
    while (name[i] && name[i] != '.' && o < 8) out[o++] = up_c(name[i++]);
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') {
        i++;
        for (int e = 8; name[i] && e < 11; ) out[e++] = up_c(name[i++]);
    }
}

/* Free a whole cluster chain back to the FAT. */
static void free_chain(uint32_t cl) {
    uint32_t steps = 0;
    while (cl >= 2 && cl < EOC) { uint32_t nx = fat_step(cl, &steps); fat_set(cl, 0); cl = nx; }
}

static long fat32_delete(const char *name);          /* forward decl */

static long fat32_write(const char *name, const void *data, unsigned long len) {
    uint32_t dir; const char *leaf;
    if (resolve(name, &dir, &leaf) < 0 || !leaf[0]) return -1;
    fat32_delete(name);            /* replace, don't duplicate: drop any old copy */

    uint32_t csize = sec_per_clus * SECSZ;
    uint32_t nclus = (uint32_t)((len + csize - 1) / csize);
    if (nclus == 0) nclus = 1;

    /* allocate a cluster chain and write the data into it */
    uint32_t first = 0, prev = 0;
    const uint8_t *p = data;
    unsigned long rem = len;
    for (uint32_t i = 0; i < nclus; i++) {
        uint32_t cl = alloc_cluster();
        if (!cl) { free_chain(first); return -1; }    /* disk full: free partial chain */
        if (i == 0) first = cl; else fat_set(prev, cl);
        uint32_t chunk = rem > csize ? csize : (uint32_t)rem;
        write_cluster(cl, p, chunk);
        p += chunk; rem -= chunk;
        prev = cl;
    }

    uint8_t name83[11];
    to_83(leaf, name83);
    if (add_entry(dir, name83, 0x20, first, (uint32_t)len) < 0) {
        free_chain(first); return -1;                 /* dir full: don't leak the data */
    }
    return (long)len;
}

/* Create a new (empty) subdirectory at `path`. */
static long fat32_mkdir(const char *path) {
    uint32_t dir; const char *leaf;
    if (resolve(path, &dir, &leaf) < 0 || !leaf[0]) return -1;
    if (dir_find(dir, leaf, 0, 0, 0)) return -1;          /* already exists */

    uint32_t newcl = alloc_cluster();
    if (!newcl) return -1;

    /* the new directory's first cluster holds just "." and ".." (rest zero) */
    uint8_t buf[64];
    for (int i = 0; i < 64; i++) buf[i] = 0;
    for (int i = 0; i < 11; i++) { buf[i] = ' '; buf[32 + i] = ' '; }
    buf[0] = '.'; buf[11] = 0x10;                          /* "."  -> itself */
    buf[20] = newcl >> 16; buf[21] = newcl >> 24; buf[26] = newcl; buf[27] = newcl >> 8;
    buf[32] = '.'; buf[33] = '.'; buf[43] = 0x10;          /* ".." -> parent  */
    uint32_t par = (dir == root_cluster) ? 0 : dir;        /* root stored as 0 */
    buf[52] = par >> 16; buf[53] = par >> 24; buf[58] = par; buf[59] = par >> 8;
    write_cluster(newcl, buf, 64);

    uint8_t name83[11];
    to_83(leaf, name83);
    if (add_entry(dir, name83, 0x10, newcl, 0) < 0) { fat_set(newcl, 0); return -1; }
    return 0;
}

/* ---- recursive tree listing ---- */
struct tctx { char *out; int *p; int max; int depth; };
static void tree_rec(uint32_t cl, char *out, int *p, int max, int depth);

static int tree_visit(const uint8_t *e, const char *name, void *ctx) {
    struct tctx *c = ctx;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
        return 0;                                 /* skip "." and ".." */
    int *p = c->p, isdir = (e[11] & 0x10);
    for (int i = 0; i < c->depth && *p < c->max - 2; i++) { c->out[(*p)++] = ' '; c->out[(*p)++] = ' '; }
    for (int i = 0; name[i] && *p < c->max - 2; i++) c->out[(*p)++] = name[i];
    if (isdir && *p < c->max - 1) c->out[(*p)++] = '/';
    if (*p < c->max - 1) c->out[(*p)++] = '\n';
    if (isdir && c->depth < 5) {                  /* recurse (depth-capped) */
        uint32_t fc = (uint32_t)rd16(e + 26) | ((uint32_t)rd16(e + 20) << 16);
        if (fc >= 2) tree_rec(fc, c->out, c->p, c->max, c->depth + 1);
    }
    return 0;
}
static void tree_rec(uint32_t cl, char *out, int *p, int max, int depth) {
    struct tctx c = { out, p, max, depth };
    walk_dir(cl, tree_visit, &c);
}
static long fat32_tree(char *out, int max) {
    int p = 0;
    tree_rec(cwd_cluster, out, &p, max, 0);
    out[p < max ? p : max - 1] = 0;
    return p;
}

/* ---- recursive name search (`find`) ---- */
static int name_has(const char *hay, const char *needle) {   /* case-insensitive substring */
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (hay[i+j] && needle[j] && up(hay[i+j]) == up(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}
struct findctx { const char *want; char *path; int plen; char *out; int *p; int max; int depth; };
static void find_rec(uint32_t cl, struct findctx *c);
static int find_visit(const uint8_t *e, const char *name, void *ctx) {
    struct findctx *c = ctx;
    if (name[0]=='.' && (name[1]==0 || (name[1]=='.' && name[2]==0))) return 0;
    int isdir = (e[11] & 0x10), saved = c->plen;
    if (c->plen < 200) c->path[c->plen++] = '/';
    for (int i = 0; name[i] && c->plen < 200; i++) c->path[c->plen++] = name[i];
    c->path[c->plen] = 0;
    if (name_has(name, c->want)) {                  /* a hit: emit its full path */
        for (int i = 0; i < c->plen && *c->p < c->max - 2; i++) c->out[(*c->p)++] = c->path[i];
        if (isdir && *c->p < c->max - 2) c->out[(*c->p)++] = '/';
        if (*c->p < c->max - 1) c->out[(*c->p)++] = '\n';
    }
    if (isdir && c->depth < 6) {
        uint32_t fc = (uint32_t)rd16(e + 26) | ((uint32_t)rd16(e + 20) << 16);
        if (fc >= 2) { c->depth++; find_rec(fc, c); c->depth--; }
    }
    c->plen = saved; c->path[saved] = 0;
    return 0;
}
static void find_rec(uint32_t cl, struct findctx *c) { walk_dir(cl, find_visit, c); }
static long fat32_find(const char *want, char *out, int max) {
    char path[208]; path[0] = 0;
    int p = 0;
    struct findctx c = { want, path, 0, out, &p, max, 0 };
    find_rec(cwd_cluster, &c);
    out[p < max ? p : max - 1] = 0;
    return p;
}

/* Change the current directory to `path` (so relative names resolve under it). */
static int fat32_chdir(const char *path) {
    uint32_t cur = (path[0] == '/') ? root_cluster : cwd_cluster;
    const char *p = path; if (*p == '/') p++;
    while (*p) {
        const char *slash = p; while (*slash && *slash != '/') slash++;
        char comp[16]; int n = 0;
        for (const char *q = p; q < slash && n < 15; q++) comp[n++] = *q;
        comp[n] = 0;
        if (comp[0] && !ieq(comp, ".")) {
            uint32_t fc; int isdir;
            if (ieq(comp, "..")) {
                if (dir_find(cur, "..", &fc, &isdir, 0)) cur = fc ? fc : root_cluster;
            } else {
                if (!dir_find(cur, comp, &fc, &isdir, 0) || !isdir) return -1;
                cur = fc;
            }
        }
        p = *slash ? slash + 1 : slash;
    }
    cwd_cluster = cur;
    return 0;
}

static long fat32_delete(const char *name) {
    uint32_t dir; const char *leaf;
    if (resolve(name, &dir, &leaf) < 0 || !leaf[0]) return -1;
    uint8_t want[11];
    to_83(leaf, want);
    uint8_t sec[SECSZ];
    uint32_t cl = dir, steps = 0;
    while (cl < EOC) {
        uint32_t firsts = cluster_to_sector(cl);
        for (uint32_t s = 0; s < sec_per_clus; s++) {
            if (ata_read(firsts + s, 1, sec) < 0) return -1;
            for (int off = 0; off < SECSZ; off += 32) {
                uint8_t *e = sec + off;
                if (e[0] == 0x00 || e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08))
                    continue;
                int eq = 1;
                for (int i = 0; i < 11; i++) if (e[i] != want[i]) { eq = 0; break; }
                if (!eq) continue;
                uint32_t fc = (uint32_t)rd16(e + 26) | ((uint32_t)rd16(e + 20) << 16);
                free_chain(fc);              /* free the file's cluster chain (guarded) */
                e[0] = 0xE5;                 /* mark the dir entry deleted */
                ata_write(firsts + s, 1, sec);
                return 0;
            }
        }
        cl = fat_step(cl, &steps);
    }
    return -1;
}

/* Report free + total bytes on the volume (counts unallocated FAT entries). */
static void fat32_df(uint64_t *freeb, uint64_t *totalb) {
    uint8_t sec[SECSZ];
    uint32_t per = SECSZ / 4, freecl = 0;
    for (uint32_t cl = 2; cl < total_clusters + 2; cl++) {
        uint32_t s = fat_start + (cl * 4) / SECSZ;
        if ((cl == 2) || ((cl * 4) % SECSZ == 0)) { if (ata_read(s, 1, sec) < 0) break; }
        uint32_t e = (cl * 4) % SECSZ;
        if ((rd32(sec + e) & 0x0FFFFFFF) == 0) freecl++;
        (void)per;
    }
    uint64_t cs = (uint64_t)sec_per_clus * SECSZ;
    *freeb  = (uint64_t)freecl * cs;
    *totalb = (uint64_t)total_clusters * cs;
}

static struct vfs_ops fat32_ops = { fat32_list, fat32_read, fat32_write,
                                    fat32_delete, fat32_mkdir, fat32_chdir,
                                    fat32_tree, fat32_df, fat32_find };

int fat32_mount(void) {
    uint8_t bs[SECSZ];
    if (ata_read(0, 1, bs) < 0)
        return -1;

    if (rd16(bs + 510) != 0xAA55)          /* boot signature */
        return -1;
    bytes_per_sec = rd16(bs + 11);
    sec_per_clus  = bs[13];
    if (bytes_per_sec != SECSZ || sec_per_clus == 0)
        return -1;

    uint32_t reserved = rd16(bs + 14);
    num_fats    = bs[16];
    fat_sectors = rd32(bs + 36);           /* FAT size 32 */
    if (fat_sectors == 0)
        return -1;                          /* not FAT32 */
    root_cluster = rd32(bs + 44);
    cwd_cluster  = root_cluster;           /* start in the root directory */

    fat_start  = reserved;
    data_start = reserved + num_fats * fat_sectors;

    uint32_t tot_sec = rd16(bs + 19) ? rd16(bs + 19) : rd32(bs + 32);  /* 16- or 32-bit count */
    uint32_t meta = reserved + num_fats * fat_sectors;
    total_clusters = (tot_sec > meta) ? (tot_sec - meta) / sec_per_clus : 0;

    vfs_register(&fat32_ops);
    return 0;
}
