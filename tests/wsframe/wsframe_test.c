/* wsframe_test.c — host-side regression for the RFC 6455 frame codec
 * (kernel/wsframe.h): ws_build_client_frame + ws_parse_frame. Pure, built for
 * the host under ASan+UBSan. Exit 0 = pass. Keep in sync with kernel/wsframe.h.
 *
 * Coverage: masked round-trips across all three payload-length encodings
 * (7-bit / 16-bit / 64-bit), the exact on-wire header bytes, unmasking, the
 * incomplete-buffer ("need more") signal at every header boundary, oversized
 * rejection, control opcodes, and a fuzz sweep that builds a random frame and
 * parses it back byte-for-byte. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "wsframe.h"

static int fails = 0, checks = 0;
#define OK(cond) do { checks++; if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

/* Build `msg` (len bytes) as a masked TEXT frame with key `k`, then parse it
 * back and assert the recovered payload matches exactly. */
static void roundtrip(const uint8_t *msg, uint64_t len, const uint8_t k[4], size_t want_hdr) {
    uint8_t frame[70000], back[70000];
    long fl = ws_build_client_frame(WS_OP_TEXT, msg, len, k, frame, sizeof frame);
    OK(fl > 0);
    OK((uint64_t)fl == want_hdr + len);              /* header size we expect */
    OK((frame[0] & 0x80) != 0 && (frame[0] & 0x0f) == WS_OP_TEXT);
    OK((frame[1] & 0x80) != 0);                      /* client frames are masked */

    int fin = -1, op = -1; uint64_t pl = 0; size_t used = 0;
    int r = ws_parse_frame(frame, (size_t)fl, &fin, &op, back, sizeof back, &pl, &used);
    OK(r == 1);
    OK(fin == 1 && op == WS_OP_TEXT);
    OK(pl == len);
    OK(used == (size_t)fl);
    OK(memcmp(back, msg, len) == 0);
}

int main(void) {
    uint8_t key[4] = { 0x37, 0xfa, 0x21, 0x3d };

    /* --- known-answer masking: RFC 6455 §5.7 masked "Hello" example --- */
    {
        uint8_t f[32];
        uint8_t mk[4] = { 0x37, 0xfa, 0x21, 0x3d };
        long fl = ws_build_client_frame(WS_OP_TEXT, (const uint8_t *)"Hello", 5, mk, f, sizeof f);
        OK(fl == 11);
        /* 0x81 0x85 <mask> 7f 9f 4d 51 58 (the spec's masked bytes) */
        OK(f[0] == 0x81 && f[1] == 0x85);
        OK(f[2] == 0x37 && f[3] == 0xfa && f[4] == 0x21 && f[5] == 0x3d);
        OK(f[6] == 0x7f && f[7] == 0x9f && f[8] == 0x4d && f[9] == 0x51 && f[10] == 0x58);
    }

    /* --- header sizes across the three length classes --- */
    {
        uint8_t small[125], mid[126];
        memset(small, 'a', sizeof small); memset(mid, 'b', sizeof mid);
        roundtrip((const uint8_t *)"",   0,             key, 6);   /* empty: 2 + 4 mask */
        roundtrip((const uint8_t *)"hi", 2,             key, 6);   /* 7-bit len */
        roundtrip(small, 125,                           key, 6);   /* 7-bit boundary (125) */
        roundtrip(mid,   126,                           key, 8);   /* 16-bit len kicks in at 126 */
    }
    /* 16-bit upper boundary (65535) and 64-bit lower boundary (65536) */
    {
        static uint8_t buf[70000];
        memset(buf, 'z', sizeof buf);
        roundtrip(buf, 65535, key, 8);                  /* still 16-bit */
        roundtrip(buf, 65536, key, 14);                 /* 64-bit: 2 + 8 + 4 mask */
    }

    /* --- the 126/127 length prefixes are emitted correctly --- */
    {
        static uint8_t buf[70000]; memset(buf, 1, sizeof buf);
        uint8_t f[70020];
        long fl = ws_build_client_frame(WS_OP_BIN, buf, 300, key, f, sizeof f);
        OK(fl == 2 + 2 + 4 + 300);
        OK((f[1] & 0x7f) == 126);
        OK(((f[2] << 8) | f[3]) == 300);                /* big-endian 16-bit */
        fl = ws_build_client_frame(WS_OP_BIN, buf, 66000, key, f, sizeof f);
        OK(fl == 2 + 8 + 4 + 66000);
        OK((f[1] & 0x7f) == 127);
        uint64_t l = 0; for (int i = 0; i < 8; i++) l = (l << 8) | f[2 + i];
        OK(l == 66000);                                 /* big-endian 64-bit */
    }

    /* --- ws_parse_frame: "need more bytes" at each header boundary --- */
    {
        uint8_t f[16] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
        uint8_t out[16]; uint64_t pl; size_t used; int fin, op;
        OK(ws_parse_frame(f, 0, &fin, &op, out, sizeof out, &pl, &used) == 0);  /* <2 bytes */
        OK(ws_parse_frame(f, 1, &fin, &op, out, sizeof out, &pl, &used) == 0);
        OK(ws_parse_frame(f, 6, &fin, &op, out, sizeof out, &pl, &used) == 0);  /* header ok, no payload */
        OK(ws_parse_frame(f, 10, &fin, &op, out, sizeof out, &pl, &used) == 0); /* 1 payload byte short */
        OK(ws_parse_frame(f, 11, &fin, &op, out, sizeof out, &pl, &used) == 1); /* complete */
    }
    /* 126-prefix incomplete extended-length field returns "need more" */
    {
        uint8_t f[4] = { 0x81, 126, 0x01 };             /* only 3 bytes: u16 len half-present */
        uint8_t out[8]; uint64_t pl; size_t used; int fin, op;
        OK(ws_parse_frame(f, 3, &fin, &op, out, sizeof out, &pl, &used) == 0);
    }
    /* 127-prefix incomplete extended-length field returns "need more" */
    {
        uint8_t f[10] = { 0x81, 127, 0, 0, 0, 0 };      /* only 6 bytes: u64 len partial */
        uint8_t out[8]; uint64_t pl; size_t used; int fin, op;
        OK(ws_parse_frame(f, 6, &fin, &op, out, sizeof out, &pl, &used) == 0);
    }

    /* --- oversized payload (declared len > caller buffer) is rejected --- */
    {
        uint8_t f[8] = { 0x82, 10 };                    /* unmasked BIN, 10-byte payload */
        for (int i = 0; i < 6; i++) f[2 + i] = (uint8_t)i;
        uint8_t out[4]; uint64_t pl = 0; size_t used = 999; int fin = 9, op = 9;
        int r = ws_parse_frame(f, 8, &fin, &op, out, sizeof out /*=4*/, &pl, &used);
        OK(r == -1);
        OK(pl == 10);                                   /* declared length still reported */
        OK(op == WS_OP_BIN);                            /* header fields set before the reject */
    }

    /* --- an UNMASKED server frame parses (real servers don't mask) --- */
    {
        uint8_t f[8] = { 0x81, 0x03, 'a', 'b', 'c' };   /* FIN TEXT, len 3, no mask */
        uint8_t out[8]; uint64_t pl; size_t used; int fin, op;
        int r = ws_parse_frame(f, 5, &fin, &op, out, sizeof out, &pl, &used);
        OK(r == 1 && pl == 3 && used == 5 && op == WS_OP_TEXT);
        OK(memcmp(out, "abc", 3) == 0);
    }

    /* --- control opcodes survive the round-trip (close/ping/pong) --- */
    {
        uint8_t f[32], out[32]; uint64_t pl; size_t used; int fin, op;
        int ops[] = { WS_OP_CLOSE, WS_OP_PING, WS_OP_PONG };
        for (int i = 0; i < 3; i++) {
            long fl = ws_build_client_frame((uint8_t)ops[i], (const uint8_t *)"x", 1, key, f, sizeof f);
            OK(fl == 7);
            OK(ws_parse_frame(f, (size_t)fl, &fin, &op, out, sizeof out, &pl, &used) == 1);
            OK(op == ops[i] && fin == 1);
        }
    }

    /* --- fuzz: random length + payload + key, build then parse, exact match --- */
    {
        srand(20260714u);
        static uint8_t msg[2048], frame[3000], back[2048];
        for (int it = 0; it < 200000; it++) {
            int len = rand() % (int)sizeof msg;         /* 0..2047, spans 7-bit + 16-bit */
            uint8_t k[4]; for (int j = 0; j < 4; j++) k[j] = (uint8_t)rand();
            for (int j = 0; j < len; j++) msg[j] = (uint8_t)rand();
            uint8_t opc = (uint8_t)((rand() % 3) ? WS_OP_TEXT : WS_OP_BIN);
            long fl = ws_build_client_frame(opc, msg, (uint64_t)len, k, frame, sizeof frame);
            if (fl <= 0) { printf("FAIL fuzz: build len=%d\n", len); fails++; break; }
            int fin, op; uint64_t pl; size_t used;
            int r = ws_parse_frame(frame, (size_t)fl, &fin, &op, back, sizeof back, &pl, &used);
            if (r != 1 || pl != (uint64_t)len || used != (size_t)fl || op != opc ||
                (len && memcmp(back, msg, len) != 0)) {
                printf("FAIL fuzz it=%d len=%d r=%d pl=%llu\n", it, len, r, (unsigned long long)pl);
                fails++; break;
            }
        }
        checks++;
    }

    if (fails) { printf("wsframe: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("wsframe: all %d frame-codec checks passed + 200k fuzz round-trips clean\n", checks);
    return 0;
}
