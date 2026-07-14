/* wsclient_test.c — host-side regression for the WebSocket client handshake
 * helpers (kernel/wsclient.h): ws_base64, ws_build_handshake, ws_handshake_status.
 * Pure, built for the host under ASan+UBSan. Exit 0 = pass. Keep in sync with
 * kernel/wsclient.h. */
#include <stdio.h>
#include <string.h>
#include "wsclient.h"

static int fails = 0, checks = 0;
#define OK(cond) do { checks++; if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)
#define EQS(got, want) do { checks++; const char *g=(got); if (strcmp(g,(want))!=0) { printf("FAIL line %d: got \"%s\" want \"%s\"\n", __LINE__, g, (want)); fails++; } } while (0)

static const char *b64(const char *s) {
    static char out[256];
    ws_base64((const uint8_t *)s, strlen(s), out);
    return out;
}

int main(void) {
    /* --- ws_base64: the RFC 4648 §10 known-answer vectors --- */
    EQS(b64(""),       "");
    EQS(b64("f"),      "Zg==");
    EQS(b64("fo"),     "Zm8=");
    EQS(b64("foo"),    "Zm9v");
    EQS(b64("foob"),   "Zm9vYg==");
    EQS(b64("fooba"),  "Zm9vYmE=");
    EQS(b64("foobar"), "Zm9vYmFy");
    /* a 16-byte nonce (the actual Sec-WebSocket-Key size) always encodes to 24
     * chars ending in "==" (16 mod 3 == 1 -> a 1-byte final group -> two pads). */
    {
        uint8_t nonce[16]; for (int i = 0; i < 16; i++) nonce[i] = (uint8_t)(i * 17 + 3);
        char out[40]; ws_base64(nonce, 16, out);
        OK(strlen(out) == 24);
        OK(out[22] == '=' && out[23] == '=');
    }
    /* the RFC 6455 §1.3 example key: the 16 raw bytes base64 to "dGhlIHNhbXBsZSBub25jZQ==" */
    {
        const uint8_t raw[16] = { 't','h','e',' ','s','a','m','p','l','e',' ','n','o','n','c','e' };
        char out[40]; ws_base64(raw, 16, out);
        EQS(out, "dGhlIHNhbXBsZSBub25jZQ==");
    }

    /* --- ws_build_handshake: the request has the required lines --- */
    {
        char req[512];
        long n = ws_build_handshake("echo.example:8080", "/chat",
                                    "dGhlIHNhbXBsZSBub25jZQ==", req, sizeof req);
        OK(n > 0);
        OK((size_t)n == strlen(req));
        OK(strncmp(req, "GET /chat HTTP/1.1\r\n", 20) == 0);
        OK(strstr(req, "Host: echo.example:8080\r\n") != NULL);
        OK(strstr(req, "Upgrade: websocket\r\n") != NULL);
        OK(strstr(req, "Connection: Upgrade\r\n") != NULL);
        OK(strstr(req, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != NULL);
        OK(strstr(req, "Sec-WebSocket-Version: 13\r\n") != NULL);
        /* header block is terminated by a blank line */
        OK(strcmp(req + n - 4, "\r\n\r\n") == 0);
    }
    /* overflow: a tiny buffer can't hold the request -> -1, no OOB write (ASan) */
    {
        char tiny[16];
        OK(ws_build_handshake("h", "/", "k", tiny, sizeof tiny) == -1);
    }

    /* --- ws_handshake_status --- */
    OK(ws_handshake_status("HTTP/1.1 101 Switching Protocols\r\n\r\n", 36) == 101);
    OK(ws_handshake_status("HTTP/1.0 101 Web Socket Protocol Handshake\r\n", 44) == 101);
    OK(ws_handshake_status("HTTP/1.1 400 Bad Request\r\n", 26) == 400);
    OK(ws_handshake_status("HTTP/1.1 404 Not Found\r\n", 24) == 404);
    OK(ws_handshake_status("HTTP/1.11 101 x\r\n", 17) == 101);   /* multi-digit minor version */
    OK(ws_handshake_status("garbage", 7) == -1);                /* too short / not HTTP */
    OK(ws_handshake_status("HTTP/1.1 xx ...", 15) == -1);        /* non-numeric code */
    OK(ws_handshake_status("", 0) == -1);

    if (fails) { printf("wsclient: %d of %d checks FAILED\n", fails, checks); return 1; }
    printf("wsclient: all %d handshake-helper checks passed (base64 KAT + request build + status parse)\n", checks);
    return 0;
}
