/*
 * speaker.c — the PC speaker.
 *
 * The speaker is wired to PIT channel 2: program channel 2 (port 0x42) as a
 * square-wave generator at the desired frequency, then gate it to the speaker
 * by setting the low two bits of port 0x61. Clear them to go silent.
 *
 * (Audible only if QEMU is started with an audio backend + `pcspk-audiodev`;
 * otherwise the programming is harmless and silent.)
 */
#include "speaker.h"
#include "io.h"
#include "timer.h"

#define PIT_CH2  0x42
#define PIT_CMD  0x43
#define SPK_PORT 0x61
#define PIT_HZ   1193182u

void speaker_tone(uint32_t hz) {
    if (hz == 0) { speaker_off(); return; }
    uint32_t div = PIT_HZ / hz;
    if (div > 0xFFFF) div = 0xFFFF;            /* channel 2's reload register is 16 bits -- an unclamped div for hz below ~19 would truncate on the port writes below and alias to an unrelated pitch instead of the lowest one */
    outb(PIT_CMD, 0xB6);                       /* channel 2, lo/hi, mode 3 */
    outb(PIT_CH2, (uint8_t)(div & 0xFF));
    outb(PIT_CH2, (uint8_t)((div >> 8) & 0xFF));
    outb(SPK_PORT, inb(SPK_PORT) | 0x03);      /* gate channel 2 to speaker */
}

void speaker_off(void) {
    outb(SPK_PORT, inb(SPK_PORT) & ~0x03);
}

void beep(uint32_t hz, uint32_t ms) {
    speaker_tone(hz);
    timer_wait(ms / 10 + 1);                   /* timer ticks at 100 Hz */
    speaker_off();
}

void speaker_chime(void) {
    static const uint32_t notes[] = { 523, 659, 784, 1047 };  /* C E G C */
    for (int i = 0; i < 4; i++)
        beep(notes[i], 90);
}
