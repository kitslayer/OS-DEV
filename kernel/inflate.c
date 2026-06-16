/*
 * inflate.c — RFC-1951 DEFLATE decompressor (the algorithm behind zlib/PNG).
 *
 * This is the classic "puff" approach: a bit reader (LSB-first), canonical
 * Huffman decoding from code lengths (counts + sorted symbols), and the
 * length/distance back-reference machinery. It handles all three block types:
 * stored, fixed-Huffman, and dynamic-Huffman. Standalone (only <stdint.h>) so
 * it builds in the kernel and is unit-testable on the host.
 */
#include "inflate.h"

typedef struct {
    const uint8_t *in; int inlen, inpos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; int outcap, outpos;
} state;

static int getbit(state *s) {
    if (s->bitcnt == 0) {
        if (s->inpos >= s->inlen) return -1;
        s->bitbuf = s->in[s->inpos++];
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1; s->bitcnt--;
    return b;
}
/* n bits, LSB first */
static int getbits(state *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        int b = getbit(s); if (b < 0) return -1;
        v |= b << i;
    }
    return v;
}

/* canonical Huffman: symbols sorted by (length, value) */
typedef struct { short count[16]; short symbol[288]; } huff;

static void build(huff *h, const uint8_t *lengths, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;                         /* unused codes don't count */
    short offs[16]; offs[0] = offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i+1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (short)i;
}
/* decode one symbol by walking lengths 1..15 (the puff method) */
static int decode(state *s, huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = getbit(s); if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count; first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const short lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

/* one compressed block, given its literal/length and distance tables */
static int inflate_block(state *s, huff *lh, huff *dh) {
    for (;;) {
        int sym = decode(s, lh); if (sym < 0) return -1;
        if (sym == 256) return 0;                       /* end of block */
        if (sym < 256) {
            if (s->outpos >= s->outcap) return -1;
            s->out[s->outpos++] = (uint8_t)sym;
        } else {
            sym -= 257; if (sym >= 29) return -1;
            int extra = getbits(s, lext[sym]); if (extra < 0) return -1;
            int len = lbase[sym] + extra;
            int dsym = decode(s, dh); if (dsym < 0 || dsym >= 30) return -1;
            int dextra = getbits(s, dext[dsym]); if (dextra < 0) return -1;
            int dist = dbase[dsym] + dextra;
            if (dist > s->outpos) return -1;            /* back-ref before start */
            if (s->outpos + len > s->outcap) return -1;
            for (int i = 0; i < len; i++) {             /* may overlap (LZ77) */
                s->out[s->outpos] = s->out[s->outpos - dist];
                s->outpos++;
            }
        }
    }
}

/* build the two fixed-Huffman tables defined by the RFC */
static void fixed_tables(huff *lh, huff *dh) {
    uint8_t l[288];
    for (int i = 0;   i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    build(lh, l, 288);
    uint8_t d[30]; for (int i = 0; i < 30; i++) d[i] = 5;
    build(dh, d, 30);
}

/* read the dynamic-Huffman header and build the lit/dist tables */
static int dynamic_tables(state *s, huff *lh, huff *dh) {
    static const uint8_t ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hlit  = getbits(s, 5); if (hlit  < 0) return -1; hlit  += 257;
    int hdist = getbits(s, 5); if (hdist < 0) return -1; hdist += 1;
    int hclen = getbits(s, 4); if (hclen < 0) return -1; hclen += 4;
    if (hlit > 286 || hdist > 30) return -1;

    uint8_t cl[19] = {0};
    for (int i = 0; i < hclen; i++) { int v = getbits(s, 3); if (v < 0) return -1; cl[ord[i]] = (uint8_t)v; }
    huff clh; build(&clh, cl, 19);

    uint8_t lengths[286 + 30] = {0};
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = decode(s, &clh); if (sym < 0) return -1;
        if (sym < 16) { lengths[n++] = (uint8_t)sym; }
        else if (sym == 16) {                           /* repeat last 3-6 times */
            if (n == 0) return -1;
            int r = getbits(s, 2); if (r < 0) return -1; r += 3;
            uint8_t prev = lengths[n-1];
            while (r-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {                          /* repeat zero 3-10 */
            int r = getbits(s, 3); if (r < 0) return -1; r += 3;
            while (r-- && n < total) lengths[n++] = 0;
        } else {                                         /* sym == 18: zero 11-138 */
            int r = getbits(s, 7); if (r < 0) return -1; r += 11;
            while (r-- && n < total) lengths[n++] = 0;
        }
    }
    if (n != total) return -1;
    build(lh, lengths, hlit);
    build(dh, lengths + hlit, hdist);
    return 0;
}

int inflate(const uint8_t *src, int srclen, uint8_t *dst, int dstcap) {
    state s = { src, srclen, 0, 0, 0, dst, dstcap, 0 };
    int final;
    do {
        final = getbit(&s); if (final < 0) return -1;
        int type = getbits(&s, 2); if (type < 0) return -1;
        if (type == 0) {                                 /* stored: byte-aligned */
            s.bitbuf = 0; s.bitcnt = 0;                  /* drop to a byte boundary */
            if (s.inpos + 4 > s.inlen) return -1;
            int len  = s.in[s.inpos] | (s.in[s.inpos+1] << 8);
            s.inpos += 4;                                /* skip LEN + NLEN */
            if (s.inpos + len > s.inlen || s.outpos + len > s.outcap) return -1;
            for (int i = 0; i < len; i++) s.out[s.outpos++] = s.in[s.inpos++];
        } else if (type == 1) {
            huff lh, dh; fixed_tables(&lh, &dh);
            if (inflate_block(&s, &lh, &dh) < 0) return -1;
        } else if (type == 2) {
            huff lh, dh;
            if (dynamic_tables(&s, &lh, &dh) < 0) return -1;
            if (inflate_block(&s, &lh, &dh) < 0) return -1;
        } else {
            return -1;                                   /* reserved block type */
        }
    } while (!final);
    return s.outpos;
}

/* gzip (RFC 1952) wrapper around inflate(): a 10-byte header, optional fields
 * selected by the flag byte, the raw DEFLATE body, then an 8-byte trailer
 * (CRC32 + ISIZE) that inflate() simply ignores. Every header read is bounded
 * against `len`, so a truncated/adversarial header can't over-read. */
int gz_inflate(const uint8_t *gz, int len, uint8_t *dst, int dstcap) {
    if (len < 18) return -1;                          /* 10 hdr + >=0 body + 8 trailer */
    if (gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8) return -1;   /* magic + DEFLATE method */
    int flg = gz[3];
    int p = 10;
    if (flg & 0x04) {                                 /* FEXTRA: 2-byte length + that many bytes */
        if (p + 2 > len) return -1;
        int xlen = gz[p] | (gz[p + 1] << 8);
        p += 2 + xlen;
    }
    if (flg & 0x08) { while (p < len && gz[p]) p++; p++; }   /* FNAME: NUL-terminated */
    if (flg & 0x10) { while (p < len && gz[p]) p++; p++; }   /* FCOMMENT: NUL-terminated */
    if (flg & 0x02) p += 2;                                  /* FHCRC: 2-byte header CRC */
    if (p < 0 || p >= len) return -1;                 /* header ran off the end */
    return inflate(gz + p, len - p, dst, dstcap);     /* DEFLATE body (trailer ignored) */
}
