/*
 * module.h — loadable kernel modules (M1261).
 *
 * A real in-kernel ELF loader for relocatable (ET_REL) object files — the
 * insmod/.ko mechanism. Given a .ko image it: copies the module's allocatable
 * sections into an executable area, resolves the module's undefined symbols
 * against the kernel symbol table (kernel/ksyms.c — the same table the panic
 * backtrace uses), applies the ELF relocations (R_X86_64_64 / PC32 / PLT32 /
 * 32 / 32S), finds the module's `mod_init` entry point and calls it.
 *
 * The demo module kernel/testmod.c is compiled to build/testmod.ko and incbin'd
 * into the kernel image (kernel/asm/mod_blob.asm), so module_load_builtin()
 * exercises the whole pipeline with no filesystem. (Loading a .ko from a file is
 * a natural follow-on once a binary can be placed on the disk.)
 */
#pragma once
#include <stdint.h>

/* Load an ET_REL module image (len bytes): relocate it, resolve its kernel
 * imports, and call mod_init(). Returns mod_init's int return value (>=0), or a
 * negative error code (unresolved symbol, bad ELF, no room, etc.). */
long module_load(const void *image, unsigned long len);

/* Load the kernel's built-in demo module (the incbin'd build/testmod.ko). */
long module_load_builtin(void);

/* rmmod: call the named module's mod_exit (if any) and free its slot; 0/-1 (M1262). */
int  module_unload(const char *name);

/* /proc/modules: one "name size" line per loaded module; bytes written (M1262). */
int  module_list(char *buf, int max);
