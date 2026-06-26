/* reader.h — reader-mode main-content extraction (see reader.c). */
#pragma once

/* Locate a page's main-content region for reader-mode rendering. Scans body[0..len)
 * (raw HTML, possibly hostile) for a main-content container — semantic <main> /
 * <article>, ARIA role="main", or a well-known content id (#content, #mw-content-text,
 * …) — and, if one is found whose inner text is substantial, sets the lo and hi
 * byte range [lo,hi) (offsets into body) and returns 1. Returns 0 if no clear container
 * is found (caller renders the whole page). Pure; every read is bounded by len. */
int reader_main_region(const char *body, int len, int *lo, int *hi);
