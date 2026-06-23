#!/bin/sh
# gen_ksyms.sh — turn `nm -n <kernel.elf>` (on stdin) into a C symbol table for
# the kernel panic backtrace (kernel/ksyms.c). We keep only function symbols
# (nm type 't'/'T', i.e. .text) since a backtrace symbolizes return addresses;
# the table lands in .rodata (after .text) so embedding it never shifts a
# function address. `nm -n` already sorts ascending by address, which is what
# ksym_lookup()'s binary search needs.
echo '#include "ksyms.h"'
echo 'const struct ksym ksyms[] = {'
awk 'NF==3 && ($2=="t" || $2=="T") { printf "{0x%sUL,\"%s\"},\n", $1, $3 }'
echo '};'
echo 'const int ksyms_count = (int)(sizeof(ksyms)/sizeof(ksyms[0]));'
