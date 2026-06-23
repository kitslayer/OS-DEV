/*
 * measure.c — software measured boot: a hash chain over the code that ran.
 * See measure.h. PCR = SHA256(PCR || SHA256(bytes)); an append-only event log
 * makes the chain replayable. Built entirely on the kernel's SHA-256.
 */
#include "measure.h"
#include "sha256.h"
#include <stdint.h>

static uint8_t  g_pcr[MEASURE_NPCR][32];
struct mevent { int pcr; uint8_t hash[32]; char name[20]; };
static struct mevent g_log[64];
static int g_nlog;
static int g_inited;

void measure_init(void) {
    for (int i = 0; i < MEASURE_NPCR; i++)
        for (int b = 0; b < 32; b++) g_pcr[i][b] = 0;
    g_nlog = 0;
    g_inited = 1;
}

void measure_extend(int pcr, const void *data, uint64_t len, const char *name) {
    if (!g_inited) measure_init();
    if (pcr < 0 || pcr >= MEASURE_NPCR || !data) return;

    uint8_t mhash[32];
    sha256((const uint8_t *)data, (unsigned long)len, mhash);   /* the measurement */

    uint8_t cat[64];                                            /* PCR || measurement */
    for (int b = 0; b < 32; b++) { cat[b] = g_pcr[pcr][b]; cat[32 + b] = mhash[b]; }
    sha256(cat, 64, g_pcr[pcr]);                                /* fold: PCR = SHA256(PCR || m) */

    if (g_nlog < (int)(sizeof g_log / sizeof g_log[0])) {       /* append to the event log */
        g_log[g_nlog].pcr = pcr;
        for (int b = 0; b < 32; b++) g_log[g_nlog].hash[b] = mhash[b];
        int k = 0; if (name) while (name[k] && k < 19) { g_log[g_nlog].name[k] = name[k]; k++; }
        g_log[g_nlog].name[k] = 0;
        g_nlog++;
    }
}

/* --- text formatting for /proc/measure ------------------------------------- */
static int m_put(char *b, int p, int max, const char *s) {
    while (*s && p + 1 < max) b[p++] = *s++;
    return p;
}
static int m_hex(char *b, int p, int max, const uint8_t *h, int n) {
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < n && p + 2 < max; i++) { b[p++] = H[h[i] >> 4]; b[p++] = H[h[i] & 15]; }
    return p;
}
static int m_dec(char *b, int p, int max, uint64_t v) {
    char t[24]; int ti = 0;
    if (!v) t[ti++] = '0'; else while (v) { t[ti++] = (char)('0' + v % 10); v /= 10; }
    while (ti && p + 1 < max) b[p++] = t[--ti];
    return p;
}

int measure_format(char *out, int max) {
    int p = 0;
    p = m_put(out, p, max, "software measured boot  (PCR = SHA256(PCR || measurement))\n");
    static const char *lbl[MEASURE_NPCR] = { "PCR0 kernel ", "PCR1 apps   ", "PCR2        ", "PCR3        " };
    for (int i = 0; i < MEASURE_NPCR; i++) {
        p = m_put(out, p, max, lbl[i]); p = m_put(out, p, max, ": ");
        p = m_hex(out, p, max, g_pcr[i], 32); p = m_put(out, p, max, "\n");
    }
    p = m_put(out, p, max, "event log ("); p = m_dec(out, p, max, (uint64_t)g_nlog);
    p = m_put(out, p, max, " entries, replay to verify the PCRs):\n");
    for (int i = 0; i < g_nlog; i++) {
        p = m_put(out, p, max, "  PCR"); p = m_dec(out, p, max, (uint64_t)g_log[i].pcr);
        p = m_put(out, p, max, "  ");
        int s = p; p = m_put(out, p, max, g_log[i].name);       /* pad the name to a column */
        for (int c = p - s; c < 12 && p + 1 < max; c++) out[p++] = ' ';
        p = m_hex(out, p, max, g_log[i].hash, 32);
        p = m_put(out, p, max, "\n");
    }
    if (p < max) out[p] = 0;
    return p;
}
