/*
 * zip.c — a from-scratch PKZIP/.zip archive EXTRACTOR (no encryption).
 *
 * The .zip container (PKWARE APPNOTE) is a sequence of per-file Local File
 * Headers + compressed data, followed by a Central Directory (one record per
 * file) and an End Of Central Directory (EOCD) record. We parse via the central
 * directory: it is the authoritative index — it always stores the real
 * compressed/uncompressed sizes and the byte offset of each local header, so we
 * never have to guess at streaming "data descriptors" (whose presence is only
 * hinted by a flag bit and whose sizes may be zero in the local header).
 *
 * Flow:
 *   1. Scan BACKWARD from end-of-file for the EOCD signature "PK\5\6"
 *      (0x06054b50). It lives in the last ~64KB+22 bytes; a variable-length
 *      archive comment may trail it, hence the backward scan. From it we read
 *      the central-directory size/offset and the total entry count.
 *   2. Walk the central directory: each "PK\1\2" (0x02014b50) record yields the
 *      compression method, compressed & uncompressed sizes, the name/extra/
 *      comment field lengths, and the relative offset of the local header.
 *   3. For each entry seek to its "PK\3\4" (0x04034b50) local header, skip that
 *      header's OWN name+extra fields to reach the compressed data, then:
 *        method 0 (stored)  -> copy `compressed size` bytes into scratch
 *        method 8 (deflate) -> inflate() into scratch (kernel/inflate.c)
 *        other              -> skip the entry
 *      and emit(name, decoded bytes). scratch is reused for each entry.
 *   4. Directory entries (name ends in '/') are skipped, not emitted.
 *
 * BOUNDED FOR UNTRUSTED INPUT: every offset and length read from the archive is
 * attacker-controlled, so each is checked against `ziplen` before any
 * dereference; every write into scratch is checked against `scratchcap`; the
 * central-directory walk is capped by the declared entry count AND by ziplen.
 * All multi-byte fields are little-endian. Standalone (<stdint.h>/<stddef.h>).
 */
#include "zip.h"
#include "inflate.h"
#include <stddef.h>

/* ---- record signatures (little-endian 32-bit) ---- */
#define SIG_LOCAL   0x04034b50u   /* "PK\3\4" local file header           */
#define SIG_CENTRAL 0x02014b50u   /* "PK\1\2" central directory header    */
#define SIG_EOCD    0x06054b50u   /* "PK\5\6" end of central directory    */

/* ---- fixed record sizes (bytes, excluding variable-length trailing fields) */
#define EOCD_MIN_LEN     22       /* EOCD without any archive comment      */
#define CENTRAL_HDR_LEN  46       /* central dir header before name/extra/comment */
#define LOCAL_HDR_LEN    30       /* local file header before name/extra   */

/* The EOCD's comment length is a 16-bit field, so the EOCD signature can be at
 * most 22 + 65535 bytes from end of file. Bound the backward scan accordingly. */
#define EOCD_MAX_SCAN    (EOCD_MIN_LEN + 0xFFFF)

/* ---- little-endian field reads (each bounds-checked by the caller) ---- */
static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* True if [off, off+need) lies wholly within [0, total). `off` and `need` are
 * unsigned so attacker values can't wrap into a negative; we compare in 64-bit
 * to make overflow impossible. */
static int in_bounds(uint64_t off, uint64_t need, uint32_t total) {
    return off <= (uint64_t)total && need <= (uint64_t)total - off;
}

int zip_extract(const uint8_t *zip, int ziplen,
                zip_emit_fn emit, void *ctx,
                uint8_t *scratch, int scratchcap) {
    if (!zip || !emit || ziplen < EOCD_MIN_LEN || scratchcap < 0)
        return -1;
    uint32_t zlen = (uint32_t)ziplen;

    /* --- 1. Locate the EOCD by scanning backward for its signature. The
     * earliest position it could start is max(0, zlen - EOCD_MAX_SCAN); the
     * latest is zlen - EOCD_MIN_LEN. Walk downward and take the first match
     * whose declared comment length is consistent with the file end. */
    uint32_t scan_lo = 0;
    if (zlen > EOCD_MAX_SCAN) scan_lo = zlen - EOCD_MAX_SCAN;
    uint32_t eocd = 0;
    int found = 0;
    for (uint32_t p = zlen - EOCD_MIN_LEN; ; p--) {
        if (rd32(zip + p) == SIG_EOCD) {
            uint32_t comment_len = rd16(zip + p + 20);
            /* The 22-byte record plus its comment must reach exactly to (or
             * within) end of file; a self-consistent EOCD ends the search. */
            if ((uint64_t)p + EOCD_MIN_LEN + comment_len <= (uint64_t)zlen) {
                eocd = p; found = 1; break;
            }
        }
        if (p == scan_lo) break;           /* bounded: never read below scan_lo */
    }
    if (!found) return -1;

    /* EOCD fields we use:
     *   off  4 : total entries on this disk      (uint16)
     *   off 10 : total entries overall           (uint16)
     *   off 12 : size of central directory       (uint32)
     *   off 16 : offset of central dir from start (uint32) */
    uint32_t total_entries = rd16(zip + eocd + 10);
    uint32_t cd_size       = rd32(zip + eocd + 12);
    uint32_t cd_off        = rd32(zip + eocd + 16);

    /* The central directory must lie within the file (and before the EOCD). */
    if (!in_bounds(cd_off, cd_size, zlen)) return -1;
    if ((uint64_t)cd_off + cd_size > (uint64_t)eocd) return -1;

    /* --- 2. Walk the central directory, capped by total_entries AND by the
     * directory's byte span (a malformed count can't cause an over-read; a
     * malformed record that runs past cd end aborts). */
    uint32_t pos = cd_off;
    uint32_t cd_end = cd_off + cd_size;
    int emitted = 0;

    for (uint32_t i = 0; i < total_entries; i++) {
        /* Fixed 46-byte central header must fit within the directory span. */
        if (!in_bounds(pos, CENTRAL_HDR_LEN, zlen)) return -1;
        if ((uint64_t)pos + CENTRAL_HDR_LEN > (uint64_t)cd_end) return -1;
        const uint8_t *c = zip + pos;
        if (rd32(c) != SIG_CENTRAL) return -1;

        /* Central directory header fields (offsets within the record):
         *    8 : general purpose bit flag   (uint16)  [bit 0 = encrypted]
         *   10 : compression method         (uint16)
         *   20 : compressed size            (uint32)
         *   24 : uncompressed size          (uint32)
         *   28 : file name length           (uint16)
         *   30 : extra field length         (uint16)
         *   32 : file comment length        (uint16)
         *   42 : relative offset of local header (uint32) */
        uint32_t gp_flag    = rd16(c + 8);
        uint32_t method     = rd16(c + 10);
        uint32_t comp_size  = rd32(c + 20);
        uint32_t uncomp_size= rd32(c + 24);
        uint32_t name_len   = rd16(c + 28);
        uint32_t extra_len  = rd16(c + 30);
        uint32_t comment_len= rd16(c + 32);
        uint32_t local_off  = rd32(c + 42);

        /* The name field follows the fixed header; bound it before reading. */
        if (!in_bounds(pos + CENTRAL_HDR_LEN, name_len, zlen)) return -1;
        const uint8_t *name = c + CENTRAL_HDR_LEN;

        /* Advance `pos` past this whole variable-length record (header + name
         * + extra + comment), checked so the running offset can't wrap/overrun.*/
        uint64_t rec = (uint64_t)CENTRAL_HDR_LEN + name_len + extra_len + comment_len;
        if ((uint64_t)pos + rec > (uint64_t)cd_end) return -1;
        pos += (uint32_t)rec;

        /* Skip directory entries (name ends in '/') — never emitted. */
        int is_dir = (name_len > 0 && name[name_len - 1] == '/');
        if (is_dir) continue;

        /* Skip encrypted entries and unsupported methods (only 0/8 supported). */
        if (gp_flag & 0x0001) continue;
        if (method != 0 && method != 8) continue;

        /* --- 3. Seek to this entry's LOCAL header and skip its own variable
         * name+extra fields to reach the compressed data. The local header's
         * name/extra lengths are independent of the central record's, so read
         * them from the local header itself. */
        if (!in_bounds(local_off, LOCAL_HDR_LEN, zlen)) return -1;
        const uint8_t *lh = zip + local_off;
        if (rd32(lh) != SIG_LOCAL) return -1;
        uint32_t lh_name_len  = rd16(lh + 26);
        uint32_t lh_extra_len = rd16(lh + 28);

        uint64_t data_off64 = (uint64_t)local_off + LOCAL_HDR_LEN
                            + lh_name_len + lh_extra_len;
        /* The compressed payload [data_off, data_off+comp_size) must fit. */
        if (data_off64 > (uint64_t)zlen) return -1;
        uint32_t data_off = (uint32_t)data_off64;
        if (!in_bounds(data_off, comp_size, zlen)) return -1;
        const uint8_t *cdata = zip + data_off;

        int decoded_len;
        if (method == 0) {
            /* Stored: the data is the file verbatim; copy comp_size bytes
             * (which for stored == uncompressed size) into scratch. */
            if ((uint64_t)comp_size > (uint64_t)scratchcap) return -1;
            for (uint32_t k = 0; k < comp_size; k++) scratch[k] = cdata[k];
            decoded_len = (int)comp_size;
        } else {
            /* Method 8: raw DEFLATE. inflate() stops at the final block, so the
             * (possibly slightly larger) comp_size span is a safe upper bound on
             * input. Output is bounded by both the declared uncompressed size
             * and scratchcap so a lying header cannot overflow scratch. */
            int out_cap = scratchcap;
            if ((uint64_t)uncomp_size < (uint64_t)out_cap)
                out_cap = (int)uncomp_size;
            if (comp_size > (uint32_t)0x7fffffff) return -1;
            decoded_len = inflate(cdata, (int)comp_size, scratch, out_cap);
            if (decoded_len < 0) return -1;
            /* Cross-check against the central directory's uncompressed size:
             * a clean archive matches exactly. */
            if ((uint32_t)decoded_len != uncomp_size) return -1;
        }

        if (name_len > (uint32_t)0x7fffffff) return -1;
        emit(ctx, (const char *)name, (int)name_len, scratch, decoded_len);
        emitted++;
    }

    return emitted;
}
