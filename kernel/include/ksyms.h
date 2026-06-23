/*
 * ksyms.h — kernel symbol table + panic backtrace (M1078).
 *
 * `ksyms[]` is a build-time, address-sorted table of every kernel function
 * (generated from `nm` by tools/gen_ksyms.sh and embedded in .rodata via a
 * two-pass link, so a panic can print "function+offset" frames instead of raw
 * hex). ksym_lookup() binary-searches it; backtrace() walks the saved frame
 * pointers (the kernel is built -fno-omit-frame-pointer) and symbolizes each
 * return address.
 */
#pragma once

struct ksym { unsigned long addr; const char *name; };
extern const struct ksym ksyms[];      /* sorted ascending by addr (nm -n) */
extern const int ksyms_count;

/* Nearest symbol at or below `addr`; sets *off_out to addr-symbol. NULL if the
 * table is empty / addr precedes the first symbol. */
const char *ksym_lookup(unsigned long addr, unsigned long *off_out);

/* Print a symbolized call trace starting at `rip`, walking the rbp frame chain. */
void backtrace(unsigned long rip, unsigned long rbp);
