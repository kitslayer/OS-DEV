/* color.h — CSS colour parsing over untrusted page bytes (see color.c).
 *
 * parse_color reads a colour token straight from page CSS / a style="" attribute
 * (`#rgb`, `#rrggbb`, `rgb()/rgba()`, `hsl()/hsla()`, or a named colour) — all
 * attacker-controlled. It's length-bounded (never reads past v[0..vl)) and the
 * rgb/hsl integer math is clamped against overflow. Split out of browser.c
 * (M581) so it can be fuzzed in isolation. Returns 0x01000000|RGB, or 0 if the
 * token isn't a recognized colour. */
#pragma once
#include <stdint.h>

uint32_t parse_color(const char *v, int vl);
