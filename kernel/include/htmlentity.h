/* htmlentity.h — decode HTML character references from untrusted page bytes.
 *
 * Extracted from browser.c so it can be host-fuzzed in isolation (tests/htmlentfuzz):
 * it reads raw page HTML, so every scan is length-bounded and numeric refs are
 * clamped. Pure: no allocation, no globals. */
#pragma once

/* Map a Unicode code point to its nearest ASCII rendering (the renderer is
 * ASCII-only): printable ASCII passes through, common punctuation/Latin-1
 * letters fold to a base char, everything else -> ' '. */
char uni_to_ascii(unsigned v);

/* Decode the entity at s (s[0] must be '&'), reading at most maxlen bytes.
 * Writes the decoded ASCII char to *out and returns the number of input bytes
 * consumed (including the ';'), or 0 if s is not a complete decodable entity.
 * Handles &name;, &#decimal;, and &#xHEX;. Safe on arbitrary untrusted input. */
int decode_entity(const char *s, int maxlen, char *out);
