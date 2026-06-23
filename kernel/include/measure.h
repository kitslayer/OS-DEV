/*
 * measure.h — software measured boot (M1096).
 *
 * A TPM-style chain of integrity measurements. Each "PCR" (Platform
 * Configuration Register) folds in a measurement irreversibly:
 *
 *     PCR = SHA256(PCR || SHA256(measured bytes))
 *
 * Starting from all-zero, the running PCR value depends on EVERY measurement
 * and their ORDER, so it is a single 32-byte fingerprint of exactly which code
 * this boot loaded — and it cannot be rewound (you can extend, never un-extend).
 * An append-only event log records each (pcr, name, measurement-hash) so a
 * verifier can REPLAY the log and must reproduce the final PCR. This is the
 * software core of measured boot / remote attestation, built on the kernel's
 * own SHA-256. Pure bookkeeping: it reads bytes and hashes them, nothing else.
 */
#pragma once
#include <stdint.h>

#define MEASURE_NPCR 4
#define PCR_KERNEL   0     /* the kernel .text+.rodata image (incl. embedded app ELFs) */
#define PCR_APPS     1     /* every app ELF loaded, in launch order */

void measure_init(void);   /* zero the PCRs + clear the event log */

/* Measure `len` bytes at `data` into PCR `pcr`: hash them, fold the hash into
 * the PCR, and append {pcr, name, hash} to the event log. `name` is a short
 * label for the log (e.g. "kernel", or an app title). */
void measure_extend(int pcr, const void *data, uint64_t len, const char *name);

/* Format the PCRs + event log into `out` (capacity `max`) as text; returns the
 * byte length written. Backs /proc/measure. */
int  measure_format(char *out, int max);
