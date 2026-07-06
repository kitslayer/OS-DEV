/*
 * dlext_lib.c — the "dependent" half of the M1539 cross-object dynamic-
 * linking test. base_mul is NOT defined here: it's an undefined (SHN_UNDEF)
 * import that must be resolved against an earlier-dlopen'd object (dlbase.so)
 * by dl_resolve_import() in user/ulib.c. The function pointer initializer
 * (rather than a bare call) mirrors dltest_lib.c's own pattern and keeps
 * this from requiring real PLT-stub generation in the loader.
 */
extern int base_mul(int a, int b);
static int (*g_base_mul)(int, int) = base_mul;

int ext_mac(int a, int b, int c) { return g_base_mul(a, b) + c; }
