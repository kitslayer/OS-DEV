/* arccore.h — a pure archive-listing engine (M1707).
 *
 * Lists the entries (name + uncompressed size) of a ZIP or TAR archive WITHOUT
 * extracting it — a capability the OS didn't have (the shell can only extract-
 * all via unzip/tar). ZIP is read from its End-Of-Central-Directory + central
 * directory; TAR by walking its 512-byte headers. gzip'd archives (.tgz/.tar.gz)
 * are handled by the caller (user/garc.c gunzips first, then lists the tar).
 *
 * Pure (no syscalls), so it is host-unit-tested by tests/arc exactly like calc/
 * sheet/plot/gjson/diff's cores. Only reads the input buffer; never writes it.
 */
#ifndef ARCCORE_H
#define ARCCORE_H

#define ARC_MAXENT 512

typedef struct { char name[100]; long size; int isdir; } arc_entry;

static arc_entry arc_ent[ARC_MAXENT];
static int       arc_n;

enum { ARC_NONE, ARC_ZIP, ARC_TAR, ARC_GZIP };

static unsigned      arc_u16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long arc_u32(const unsigned char *p) { return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24); }

static int arc_detect(const unsigned char *d, long n) {
    if (n >= 2 && d[0] == 0x1f && d[1] == 0x8b) return ARC_GZIP;
    if (n >= 4 && d[0] == 'P' && d[1] == 'K' && (d[2] == 3 || d[2] == 5 || d[2] == 1)) return ARC_ZIP;
    if (n >= 262 && d[257] == 'u' && d[258] == 's' && d[259] == 't' && d[260] == 'a' && d[261] == 'r') return ARC_TAR;
    return ARC_NONE;
}

/* ZIP: find the EOCD record, then walk the central directory. */
static int arc_zip(const unsigned char *d, long n) {
    long i = n - 22;
    if (i < 0) return -1;
    for (; i >= 0; i--) if (d[i] == 'P' && d[i + 1] == 'K' && d[i + 2] == 5 && d[i + 3] == 6) break;
    if (i < 0) return -1;
    const unsigned char *eocd = d + i;
    int count = (int)arc_u16(eocd + 10);
    unsigned long cdoff = arc_u32(eocd + 16);
    if (cdoff >= (unsigned long)n) return -1;
    const unsigned char *p = d + cdoff;
    arc_n = 0;
    for (int e = 0; e < count && arc_n < ARC_MAXENT; e++) {
        if (p + 46 > d + n) break;
        if (!(p[0] == 'P' && p[1] == 'K' && p[2] == 1 && p[3] == 2)) break;   /* central-dir header sig */
        unsigned long usize = arc_u32(p + 24);
        unsigned nlen = arc_u16(p + 28), elen = arc_u16(p + 30), clen = arc_u16(p + 32);
        int k = 0;
        for (; k < (int)nlen && k < 99 && p + 46 + k < d + n; k++) arc_ent[arc_n].name[k] = (char)p[46 + k];
        arc_ent[arc_n].name[k] = 0;
        arc_ent[arc_n].size = (long)usize;
        arc_ent[arc_n].isdir = (k > 0 && arc_ent[arc_n].name[k - 1] == '/');
        arc_n++;
        p += 46 + nlen + elen + clen;
    }
    return arc_n;
}

/* parse an octal ASCII field (space/NUL padded), used for TAR sizes */
static long arc_octal(const unsigned char *p, int len) {
    long v = 0; int i = 0;
    while (i < len && p[i] == ' ') i++;
    for (; i < len && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (p[i] - '0');
    return v;
}

/* TAR: walk 512-byte headers until a zero block or the end. */
static int arc_tar(const unsigned char *d, long n) {
    arc_n = 0;
    long off = 0;
    while (off + 512 <= n && arc_n < ARC_MAXENT) {
        const unsigned char *h = d + off;
        int allzero = 1;
        for (int i = 0; i < 512; i++) if (h[i]) { allzero = 0; break; }
        if (allzero) break;
        long size = arc_octal(h + 124, 12);
        char tf = (char)h[156];
        if (h[0]) {                                    /* skip pax/global headers with empty names */
            int k = 0; for (; k < 99 && h[k]; k++) arc_ent[arc_n].name[k] = (char)h[k];
            arc_ent[arc_n].name[k] = 0;
            arc_ent[arc_n].size = (tf == '5') ? 0 : size;
            arc_ent[arc_n].isdir = (tf == '5');
            arc_n++;
        }
        long blocks = (size + 511) / 512;
        off += 512 + blocks * 512;
    }
    return arc_n;
}

/* List `d`'s entries into arc_ent[0..arc_n). Returns the count, or -1 if the
 * buffer isn't a (plain) ZIP or TAR (gzip must be decompressed by the caller). */
static int arc_list(const unsigned char *d, long n) {
    int f = arc_detect(d, n);
    if (f == ARC_ZIP) return arc_zip(d, n);
    if (f == ARC_TAR) return arc_tar(d, n);
    return -1;
}

#endif /* ARCCORE_H */
