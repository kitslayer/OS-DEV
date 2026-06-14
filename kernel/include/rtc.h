/* rtc.h — real-time clock (CMOS) reader. */
#pragma once

struct rtc_time { int year, month, day, hour, min, sec; };

void rtc_now(struct rtc_time *t);
