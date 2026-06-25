/*
 * testmod.c — a loadable kernel module (M1261), compiled to an ET_REL object
 * (build/testmod.ko) rather than linked into the kernel. kernel/module.c loads
 * it: copies its sections into an executable area, resolves its undefined
 * references (timer_ms) against the kernel symbol table, applies the ELF
 * relocations, then calls mod_init().
 *
 * mod_init calls an IMPORTED kernel function (forcing a relocation against an
 * undefined symbol the loader must resolve via ksyms) and touches a .data global
 * (forcing a section relocation), returning 42 iff both resolved + ran sanely.
 * Built freestanding with the kernel's own code model so its relocations match
 * what kernel/module.c handles. NOT in C_SRCS — compiled by a dedicated rule.
 */
extern unsigned long timer_ms(void);     /* imported from the kernel (resolved via ksyms) */

static volatile int g_state = 41;         /* lives in .data — exercises a section relocation */

int mod_init(void) {
    unsigned long t = timer_ms();          /* the imported symbol must link to the real kernel fn */
    return (t > 0) ? (g_state + 1) : -1;   /* 42 iff timer_ms() resolved and returned a sane uptime */
}
