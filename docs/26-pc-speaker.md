# Milestone 26 — PC speaker sound

**Goal:** make noise. The PC speaker can play square-wave tones — enough for
beeps and a startup chime.

## How (`kernel/speaker.c`)

The speaker is driven by **PIT channel 2**: program channel 2 (port 0x42) as a
square-wave generator at `1193182 / hz`, then "gate" it to the speaker by
setting the low two bits of **port 0x61**. Clear them to go silent.

- `speaker_tone(hz)` / `speaker_off()` — continuous tone on/off.
- `beep(hz, ms)` — tone for a duration (using the M3 timer to wait), then off.
- `speaker_chime()` — a little C-E-G-C arpeggio, played at startup.

A **`SYS_beep`** syscall + a shell **`beep`** command let userspace make sound.
(The syscall enables interrupts first, since `beep` waits on the timer and the
int-gate had cleared the interrupt flag.)

## Note
Audible only if QEMU runs with an audio backend (`-audiodev` +
`-machine pcspk-audiodev=…`). Without one the port writes are harmless no-ops —
so the boot chime and `beep` run fine, just silently, in the headless test.

## Files
- `kernel/speaker.c` / `speaker.h`
- `kernel/syscall.c`, `user/shell.c` — `SYS_beep` + `beep`
- `kernel/kmain.c` — startup chime
