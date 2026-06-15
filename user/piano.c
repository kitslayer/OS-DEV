/*
 * piano.c — a playable piano, a userspace program.
 *
 * Maps the keyboard to musical notes and sounds them on the PC speaker
 * (sys_beep) — the first musical use of the OS's sound hardware. The home row
 * plays the white keys (a s d f g h j k = C D E F G A B C) and the row above
 * the black keys (w e t y u = C# D# F# G# A#); z/x shift the octave. The screen
 * shows the note being played and a colour-coded key map. Runs ring-3.
 */
#include "ulib.h"

/* frequencies (Hz) for one octave, C..C, at octave 4 */
static const int FREQ[13] = { 262,277,294,311,330,349,370,392,415,440,466,494,523 };
static const char *NAME[13] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B","C" };

/* key -> semitone offset (0..12), or -1 */
static int key_note(int k) {
    switch (k) {
        case 'a': return 0;  case 'w': return 1;  case 's': return 2;  case 'e': return 3;
        case 'd': return 4;  case 'f': return 5;  case 't': return 6;  case 'g': return 7;
        case 'y': return 8;  case 'h': return 9;  case 'u': return 10; case 'j': return 11;
        case 'k': return 12; default: return -1;
    }
}

static void put_int(char *b, int *p, int v) {
    char t[8]; int i = 0; if (!v) t[i++] = '0'; while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) b[(*p)++] = t[--i];
}

static void render(int oct, int last) {        /* last = semitone last played, or -1 */
    sys_clear();
    sys_setcolor(4); print("   OS-DEV Piano\n\n");        /* title: cyan */

    sys_setcolor(8); print("   Playing:  ");
    if (last < 0) { sys_setcolor(8); print("- press a key -\n"); }
    else {
        int o = oct + (last == 12 ? 1 : 0);              /* the top key is next-octave C */
        int f = FREQ[last];
        if (oct > 4) f <<= (oct - 4); else if (oct < 4) f >>= (4 - oct);
        char b[40]; int p = 0;
        b[p++] = ' ';
        const char *n = NAME[last]; while (*n) b[p++] = *n++;
        put_int(b, &p, o);
        b[p++] = ' '; b[p++] = ' '; b[p++] = '(';
        put_int(b, &p, f);
        const char *hz = " Hz)"; while (*hz) b[p++] = *hz++;
        b[p] = 0;
        sys_setcolor(3); print(b); print("\n");          /* note: yellow */
    }
    print("\n");

    sys_setcolor(1); print("   white:  a s d f g h j k\n"); /* white keys */
    sys_setcolor(4); print("   black:   w e   t y u\n");    /* black keys: cyan */
    sys_setcolor(8); print("          C D E F G A B C\n\n");

    sys_setcolor(8); print("   z / x : octave down / up   (octave ");
    char ob[4]; int p = 0; put_int(ob, &p, oct); ob[p] = 0; print(ob); print(")\n");
    print("   q : quit\n");
    sys_setcolor(0);
}

int main(void) {
    int oct = 4, last = -1;
    render(oct, last);
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(15); continue; }
        if (k == 'q' || k == 27) return 0;
        if (k == 'z') { if (oct > 2) oct--; render(oct, last); continue; }
        if (k == 'x') { if (oct < 6) oct++; render(oct, last); continue; }
        int st = key_note(k);
        if (st < 0) continue;
        int f = FREQ[st];
        if (oct > 4) f <<= (oct - 4); else if (oct < 4) f >>= (4 - oct);
        last = st;
        render(oct, last);
        sys_beep(f, 180);                                 /* sound the note */
    }
}
