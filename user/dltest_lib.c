/*
 * dltest_lib.c — a shared library (.so) for OS-DEV's userspace dynamic linker
 * (M1263). Built `gcc -shared -fPIC -nostdlib` into an ET_DYN object, placed on
 * the FAT disk as DLTEST.SO, and loaded at runtime by ulib's dlopen()/dlsym():
 * the loader mmaps its PT_LOAD segments at a runtime base, applies the ELF
 * relocations, and resolves exported symbols from .dynsym.
 *
 * No libc, no external deps — position-independent, loadable at any base. The
 * global function pointer forces an R_X86_64_JUMP_SLOT relocation that the
 * loader MUST apply for answer()'s indirect call to land — so a correct
 * dlopen is what makes answer() return 42.
 */
static int g_bump = 2;
int  greet(int x) { return x + g_bump; }     /* RIP-relative global access */
static int (*g_fp)(int) = greet;             /* global fn-ptr -> a relocation in the GOT */
long answer(void) { return 40 + g_fp(0); }   /* indirect call through the relocated pointer -> 42 */
