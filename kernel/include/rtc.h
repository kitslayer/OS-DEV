/* rtc.h — real-time clock (CMOS) reader. */
#pragma once
#include <stdint.h>

struct rtc_time { int year, month, day, hour, min, sec; };

void rtc_now(struct rtc_time *t);
void rtc_set(const struct rtc_time *t);   /* write the CMOS clock (e.g. from SNTP) */
uint32_t rtc_unix(void);
void     rtc_selftest(void);   /* boot: concurrent CMOS readers never see a torn snapshot (M1913) */   /* current time as Unix epoch seconds — for FS timestamps (M1173) */
