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

/* Current time as Unix epoch seconds — for filesystem timestamps (statx, ext2
 * inode i_*time, tmpfs mtime). Uses Howard Hinnant's days_from_civil algorithm
 * (correct across the 1900/2100 leap-century rules). (M1173) */
uint32_t rtc_unix(void) {
    struct rtc_time t; rtc_now(&t);
    int y = t.year, m = t.month, d = t.day;
    if (m <= 2) y -= 1;                                   /* shift Jan/Feb to the end of the prior year */
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);             /* year of era [0,399] */
    unsigned doy = (unsigned)((153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1);   /* day of year [0,365] */
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; /* day of era [0,146096] */
    long days = (long)era * 146097 + (long)doe - 719468;  /* days since 1970-01-01 */
    return (uint32_t)(days * 86400 + t.hour * 3600 + t.min * 60 + t.sec);
}

static void cmos_write(uint8_t reg, uint8_t v) { outb(CMOS_ADDR, reg); outb(CMOS_DATA, v); }
static uint8_t bin2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Write the CMOS clock (used by SNTP). Honours register B's BCD/binary flag and
 * writes 24-hour (the QEMU/PC default, regB bit 1 set). Brackets the writes with
 * the SET bit so the clock isn't read mid-update. Year is the low two digits
 * (rtc_now reconstructs 2000+yy). */
void rtc_set(const struct rtc_time *t) {
    uint8_t regB = cmos(0x0B);
    int bcd = !(regB & 0x04);
    cmos_write(0x0B, regB | 0x80);                 /* SET: halt updates while we write */
    int yy = t->year % 100;
    int vals[6] = { t->sec, t->min, t->hour, t->day, t->month, yy };
    uint8_t regs[6] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09 };
    for (int i = 0; i < 6; i++)
        cmos_write(regs[i], bcd ? bin2bcd(vals[i]) : (uint8_t)vals[i]);
    cmos_write(0x0B, (uint8_t)((regB | 0x02) & ~0x80));   /* clear SET, force 24-hour mode */
}
