/* wsframe.h — RFC 6455 WebSocket frame codec (M1843).
 *
 * Pure and self-contained (only <stdint.h>/<stddef.h>), so the kernel browser
 * transport and the off-target unit test (tests/wsframe, ASan/UBSan) share the
 * exact same code. No allocation, no I/O — the caller owns every buffer.
 *
 * One frame on the wire:
 *   byte0: FIN(1) RSV(3)=0 opcode(4)
 *   byte1: MASK(1) payload-len7(7)        len7==126 => next u16, ==127 => next u64
 *   [mask key: 4 bytes, present iff MASK]
 *   payload: XOR-masked with the 4-byte key iff MASK
 *
 * A client (us) MUST mask every frame it sends (ws_build_client_frame masks
 * with the caller's key). A server MUST NOT mask; ws_parse_frame still handles
 * a mask defensively so it round-trips its own output in the unit test.
 */
#ifndef WSFRAME_H
#define WSFRAME_H
#include <stdint.h>
#include <stddef.h>

/* opcodes (RFC 6455 §5.2) */
#define WS_OP_CONT  0x0
#define WS_OP_TEXT  0x1
#define WS_OP_BIN   0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

/* Build one masked client frame (FIN=1) for `opcode` carrying `payload`
 * (`len` bytes) into `out` (capacity `outcap`), masking with the 4-byte `mask`
 * key. Returns the total frame length, or -1 if it wouldn't fit in `outcap`. */
static inline long ws_build_client_frame(uint8_t opcode, const uint8_t *payload,
                                         uint64_t len, const uint8_t mask[4],
                                         uint8_t *out, size_t outcap) {
    size_t hdr = 2;
    if (len > 65535) hdr += 8; else if (len >= 126) hdr += 2;
    hdr += 4;                                   /* mask key is always present */
    if (outcap < hdr || (uint64_t)(outcap - hdr) < len) return -1;
    size_t p = 0;
    out[p++] = (uint8_t)(0x80 | (opcode & 0x0f));      /* FIN=1 + opcode */
    if (len > 65535) {
        out[p++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) out[p++] = (uint8_t)(len >> (i * 8));
    } else if (len >= 126) {
        out[p++] = 0x80 | 126;
        out[p++] = (uint8_t)(len >> 8);
        out[p++] = (uint8_t)(len & 0xff);
    } else {
        out[p++] = (uint8_t)(0x80 | (uint8_t)len);
    }
    out[p++] = mask[0]; out[p++] = mask[1]; out[p++] = mask[2]; out[p++] = mask[3];
    for (uint64_t i = 0; i < len; i++) out[p++] = (uint8_t)(payload[i] ^ mask[i & 3]);
    return (long)p;
}

/* Build one UNMASKED server frame (FIN=1) for `opcode` carrying `payload`
 * (`len` bytes) into `out` (capacity `outcap`). RFC 6455 forbids a server from
 * masking; this is the counterpart to ws_build_client_frame for the WS server
 * (M1849). Returns the total frame length, or -1 if it wouldn't fit. */
static inline long ws_build_server_frame(uint8_t opcode, const uint8_t *payload,
                                         uint64_t len, uint8_t *out, size_t outcap) {
    size_t hdr = 2;
    if (len > 65535) hdr += 8; else if (len >= 126) hdr += 2;   /* no mask key */
    if (outcap < hdr || (uint64_t)(outcap - hdr) < len) return -1;
    size_t p = 0;
    out[p++] = (uint8_t)(0x80 | (opcode & 0x0f));      /* FIN=1 + opcode, MASK=0 */
    if (len > 65535) {
        out[p++] = 127;
        for (int i = 7; i >= 0; i--) out[p++] = (uint8_t)(len >> (i * 8));
    } else if (len >= 126) {
        out[p++] = 126;
        out[p++] = (uint8_t)(len >> 8);
        out[p++] = (uint8_t)(len & 0xff);
    } else {
        out[p++] = (uint8_t)len;
    }
    for (uint64_t i = 0; i < len; i++) out[p++] = payload[i];
    return (long)p;
}

/* Parse a single frame from the front of `in` (inlen bytes buffered).
 * Returns:
 *   1  a complete frame was parsed: *consumed = its total byte length, with
 *      *fin and *opcode set, *paylen = declared payload length, and up to
 *      min(paylen,paycap) payload bytes copied into `payload` (unmasked).
 *   0  need more bytes (header or payload not fully buffered yet); nothing else
 *      is written.
 *  -1  malformed, or the declared payload length exceeds `paycap` (the caller's
 *      buffer is too small — connection should be closed). *paylen is still set.
 * Any of fin/opcode/paylen/consumed may be NULL. */
static inline int ws_parse_frame(const uint8_t *in, size_t inlen,
                                 int *fin, int *opcode,
                                 uint8_t *payload, size_t paycap,
                                 uint64_t *paylen, size_t *consumed) {
    if (inlen < 2) return 0;
    int masked = (in[1] & 0x80) != 0;
    uint64_t len = (uint64_t)(in[1] & 0x7f);
    size_t p = 2;
    if (len == 126) {
        if (inlen < 4) return 0;
        len = ((uint64_t)in[2] << 8) | in[3];
        p = 4;
    } else if (len == 127) {
        if (inlen < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | in[2 + i];
        p = 10;
    }
    uint8_t key[4] = { 0, 0, 0, 0 };
    if (masked) {
        if (inlen < p + 4) return 0;
        for (int i = 0; i < 4; i++) key[i] = in[p + i];
        p += 4;
    }
    if (fin)    *fin    = (in[0] & 0x80) != 0;
    if (opcode) *opcode = in[0] & 0x0f;
    if (paylen) *paylen = len;
    if (len > paycap) return -1;                /* header known, payload too big */
    if ((uint64_t)(inlen - p) < len) return 0;  /* payload not fully buffered */
    for (uint64_t i = 0; i < len; i++)
        payload[i] = masked ? (uint8_t)(in[p + i] ^ key[i & 3]) : in[p + i];
    if (consumed) *consumed = p + (size_t)len;
    return 1;
}
#endif /* WSFRAME_H */
