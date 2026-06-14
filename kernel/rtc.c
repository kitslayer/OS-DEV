/*
 * rtc.c — read the CMOS real-time clock.
 *
 * The PC's battery-backed clock lives behind the CMOS index/data ports
 * (0x70/0x71): write a register number to 0x70, read its value from 0x71. The
 * time registers can be in BCD or binary and 12- or 24-hour, told by status
 * register B. We also avoid reading mid-"update" (status A bit 7) and re-read
 * until two reads agree, so we never catch the clock half-incremented.
 */
#include "rtc.h"
#include "io.h"
#include <stdint.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}
static int updating(void)      { return cmos(0x0A) & 0x80; }
static uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + (v >> 4) * 10; }

void rtc_now(struct rtc_time *t) {
    uint8_t s, mi, h, d, mo, y, ls = 0xFF;
    do {
        while (updating()) { }
        s = cmos(0x00); mi = cmos(0x02); h = cmos(0x04);
        d = cmos(0x07); mo = cmos(0x08); y = cmos(0x09);
        if (s == ls) break;             /* two consecutive reads agree */
        ls = s;
    } while (1);

    uint8_t regB = cmos(0x0B);
    int pm = h & 0x80;
    if (!(regB & 0x04)) {               /* BCD -> binary */
        s = bcd2bin(s); mi = bcd2bin(mi); h = bcd2bin(h & 0x7F);
        d = bcd2bin(d); mo = bcd2bin(mo); y = bcd2bin(y);
    } else {
        h &= 0x7F;
    }
    if (!(regB & 0x02) && pm)           /* 12-hour PM -> 24-hour */
        h = (h % 12) + 12;

    t->sec = s; t->min = mi; t->hour = h;
    t->day = d; t->month = mo; t->year = 2000 + y;
}
