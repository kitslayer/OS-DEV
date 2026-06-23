/* rtc.h — real-time clock (CMOS) reader. */
#pragma once

struct rtc_time { int year, month, day, hour, min, sec; };

void rtc_now(struct rtc_time *t);
void rtc_set(const struct rtc_time *t);   /* write the CMOS clock (e.g. from SNTP) */
