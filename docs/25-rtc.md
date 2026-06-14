# Milestone 25 — Real-time clock (RTC)

**Goal:** the OS should know the actual date and time, not just uptime.

## The CMOS RTC (`kernel/rtc.c`)

The PC's battery-backed clock is read through the CMOS index/data ports: write
a register number to **0x70**, read its value from **0x71**. Registers hold
seconds/minutes/hours/day/month/year. Two wrinkles:

- The values may be **BCD** or binary, and **12-** or 24-hour, told by CMOS
  status register B — we convert accordingly.
- The clock can be mid-**update** when you read it (status A bit 7), so we wait
  for that to clear and re-read until two reads agree — never catching it
  half-incremented.

## Where it shows up
- The **taskbar clock** now displays the real `HH:MM:SS` (updated each second).
- A new **`SYS_time`** syscall writes `YYYY-MM-DD HH:MM:SS` into a user buffer,
  and the shell gained a **`date`** command.

(QEMU feeds the RTC from the host clock, so it reads a real time.)

## Files
- `kernel/rtc.c` / `rtc.h` — the CMOS reader
- `kernel/desktop.c` — taskbar shows real time
- `kernel/syscall.c`, `user/shell.c` — `SYS_time` + `date`
