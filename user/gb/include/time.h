#ifndef _GB_TIME_H
#define _GB_TIME_H
/* Minimal time.h shim — Peanut-GB includes <time.h> but doesn't call time()
 * (it's only used by an optional RTC path we don't exercise). */
typedef long time_t;
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
static inline time_t time(time_t *t) { if (t) *t = 0; return 0; }
#endif
