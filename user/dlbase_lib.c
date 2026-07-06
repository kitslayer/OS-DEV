/*
 * dlbase_lib.c — the "dependency" half of the M1539 cross-object dynamic-
 * linking test: a plain shared library exporting a function, with nothing
 * of its own to import. Loaded FIRST (dlopen("DLBASE.SO")) so dlext_lib.c's
 * import of base_mul can resolve against it.
 */
int base_mul(int a, int b) { return a * b; }
