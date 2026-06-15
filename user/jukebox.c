/*
 * jukebox.c — plays built-in melodies on the PC speaker, a userspace program.
 *
 * Where the piano plays live keys, the jukebox plays whole tunes: press a number
 * and it beeps the melody note by note (sys_beep blocks for each note's length).
 * A second non-interactive sound app to go with the matrix screensaver. Tunes are
 * note-strings (c d e f g a b lowercase = octave 4, C = the octave above, space =
 * a rest). q quits; any key during a tune stops it. Ring-3.
 */
#include "ulib.h"

static const char *NAMES[] = { "Scale", "Twinkle Twinkle", "Ode to Joy", "Happy Birthday",
                               "Mary Had a Lamb", "Jingle Bells" };
static const char *TUNES[] = {
    "cdefgabC",
    "ccggaag ffeeddc",
    "eefggfeddccdeed",
    "ccdcfe ccdcgf",
    "edcdeee ddd egg",
    "eee eee egcde",
};
#define NT ((int)(sizeof(NAMES) / sizeof(NAMES[0])))

static int freq_of(char c) {
    switch (c) {
        case 'c': return 262; case 'd': return 294; case 'e': return 330; case 'f': return 349;
        case 'g': return 392; case 'a': return 440; case 'b': return 494; case 'C': return 523;
        default:  return 0;   /* rest */
    }
}

static void menu(int playing) {     /* playing = tune index, or -1 */
    sys_clear();
    sys_setcolor(4); print("  OS-DEV Jukebox\n\n");
    sys_setcolor(8); print("  press a number to play a tune:\n");
    for (int i = 0; i < NT; i++) {
        sys_setcolor(3); char b[6]; b[0] = ' '; b[1] = ' '; b[2] = (char)('1' + i); b[3] = ' '; b[4] = ' '; b[5] = 0;
        print(b);
        sys_setcolor(i == playing ? 9 : 1); print(NAMES[i]); print("\n");
    }
    if (playing >= 0) { sys_setcolor(9); print("\n  now playing: "); print(NAMES[playing]); print(" ...\n"); }
    sys_setcolor(8); print("\n  q to quit  (any key stops a tune)\n");
    sys_setcolor(0);
}

static void play(int t) {
    const char *tune = TUNES[t];
    menu(t);
    for (int i = 0; tune[i]; i++) {
        int f = freq_of(tune[i]);
        if (f) sys_beep(f, 280); else sys_sleep(160);   /* a note, or a rest */
        int k = sys_pollkey();
        if (k >= 0) return;                             /* any key stops the tune */
    }
}

int main(void) {
    menu(-1);
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 27) return 0;
        if (k >= '1' && k < '1' + NT) { play(k - '1'); menu(-1); }
    }
}
