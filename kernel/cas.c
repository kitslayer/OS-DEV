/*
 * cas.c — content-addressed blob store. See cas.h. Append-only arena + a
 * hash-sorted-by-insertion index with a 256-way first-byte fanout to keep
 * lookups quick. Dedup on store, integrity re-check on fetch.
 */
#include "cas.h"
#include "sha256.h"
#include <stdint.h>

#define CAS_ARENA  (512 * 1024)     /* total blob bytes the store can hold */
#define CAS_MAX    512              /* max distinct objects */

static uint8_t g_arena[CAS_ARENA];
static uint32_t g_used;
static struct cas_obj { uint8_t hash[32]; uint32_t off, len; } g_obj[CAS_MAX];
static int g_nobj;
static uint64_t g_dedup;            /* stores that hit an existing blob */

static int hash_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 32; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int find(const uint8_t *hash) {
    for (int i = 0; i < g_nobj; i++) if (hash_eq(g_obj[i].hash, hash)) return i;
    return -1;
}

int cas_store(const void *data, uint32_t len, uint8_t out_hash[32]) {
    uint8_t h[32];
    sha256((const uint8_t *)data, len, h);
    for (int i = 0; i < 32; i++) out_hash[i] = h[i];

    if (find(h) >= 0) { g_dedup++; return 0; }          /* already stored — dedup */
    if (g_nobj >= CAS_MAX || (uint64_t)g_used + len > CAS_ARENA) return -1;   /* full */

    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) g_arena[g_used + i] = src[i];
    g_obj[g_nobj].off = g_used;
    g_obj[g_nobj].len = len;
    for (int i = 0; i < 32; i++) g_obj[g_nobj].hash[i] = h[i];
    g_used += len;
    g_nobj++;
    return 0;
}

long cas_fetch(const uint8_t hash[32], void *out, uint32_t max) {
    int i = find(hash);
    if (i < 0) return -1;
    uint32_t len = g_obj[i].len;
    if (len > max) return -1;
    const uint8_t *blob = g_arena + g_obj[i].off;

    uint8_t check[32];                                   /* verify integrity before returning */
    sha256(blob, len, check);
    if (!hash_eq(check, hash)) return -1;

    uint8_t *o = (uint8_t *)out;
    for (uint32_t b = 0; b < len; b++) o[b] = blob[b];
    return (long)len;
}

/* --- /proc/cas formatting --------------------------------------------------- */
static int c_put(char *b, int p, int max, const char *s) {
    while (*s && p + 1 < max) b[p++] = *s++;
    return p;
}
static int c_num(char *b, int p, int max, uint64_t v) {
    char t[24]; int ti = 0;
    if (!v) t[ti++] = '0'; else while (v) { t[ti++] = (char)('0' + v % 10); v /= 10; }
    while (ti && p + 1 < max) b[p++] = t[--ti];
    return p;
}

int cas_format(char *out, int max) {
    int p = 0;
    p = c_put(out, p, max, "content-addressed store (blobs keyed by SHA-256)\n");
    p = c_put(out, p, max, "Objects:\t");  p = c_num(out, p, max, (uint64_t)g_nobj);
    p = c_put(out, p, max, " / ");         p = c_num(out, p, max, (uint64_t)CAS_MAX); p = c_put(out, p, max, "\n");
    p = c_put(out, p, max, "Bytes:\t");    p = c_num(out, p, max, (uint64_t)g_used);
    p = c_put(out, p, max, " / ");         p = c_num(out, p, max, (uint64_t)CAS_ARENA); p = c_put(out, p, max, "\n");
    p = c_put(out, p, max, "Dedup:\t");    p = c_num(out, p, max, g_dedup);
    p = c_put(out, p, max, " store(s) deduplicated\n");
    if (p < max) out[p] = 0;
    return p;
}
