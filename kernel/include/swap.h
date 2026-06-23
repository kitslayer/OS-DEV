/*
 * swap.h — page swapping to a disk. Anonymous (mmap) pages can be written out
 * to a backing block device and faulted back in on next access, so an app's
 * resident footprint can shrink below its mapped footprint.
 *
 * Inactive (every call a no-op) until a writable NON-boot block device is found
 * on first use — so on a normal boot with only the boot disk, swap is dormant
 * and changes nothing. The high end of the chosen device holds fixed-size page
 * slots; a slot index is packed into the swapped page's (not-present) PTE.
 */
#pragma once
#include <stdint.h>

int  swap_active(void);                 /* 1 if a swap device is configured (probes on first call) */
int  swap_out(uint64_t phys);           /* write a frame to a free slot; returns the slot, or -1 */
int  swap_in(int slot, uint64_t phys);  /* read a slot back into a frame; 0/-1 (does not free the slot) */
void swap_release(int slot);            /* free a slot (after fault-in, or on munmap of a swapped page) */
int  swap_format(char *out, int max);   /* /proc/swaps stats */
