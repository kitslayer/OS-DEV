/* speaker.h — PC speaker (square-wave tones via PIT channel 2). */
#pragma once
#include <stdint.h>

void speaker_tone(uint32_t hz);   /* start a continuous tone */
void speaker_off(void);
void beep(uint32_t hz, uint32_t ms);   /* tone for ms, then silence */
void speaker_chime(void);              /* a short startup arpeggio */
