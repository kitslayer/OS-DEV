/*
 * jukebox.c — a music player for the .WAV files on disk, a userspace program.
 *
 * Lists every .WAV file on the FAT32 disk; press its number to play it through
 * the AC'97 codec in the BACKGROUND (sys_playbg) — so the music keeps going
 * while you use other windows, and even after you quit the jukebox. [s] stops,
 * [q] quits. (The old jukebox beeped tunes on the PC speaker; now it plays real
 * audio.) Text-grid UI, ring-3.
 */
#include "ulib.h"

#define MAXW 9          /* the picker is single-key [1-9], so listing more than 9 just showed
                         * unplayable rows with garbage labels ('1'+9 = ':'); cap to the playable range */
static char wavs[MAXW][16];
static int  nwav;

/* collect the names of every .WAV file from the directory listing */
static void scan_wavs(void) {
    static char buf[8192];
    sys_list(buf, sizeof(buf));
    nwav = 0;
    char *p = buf;
    while (*p && nwav < MAXW) {
        while (*p == ' ') p++;                       /* skip leading spaces */
        char nm[16]; int k = 0;
        while (*p && *p != ' ' && *p != '\n' && k < 15) nm[k++] = *p++;
        nm[k] = 0;
        while (*p && *p != '\n') p++;                /* to the end of the line */
        if (*p == '\n') p++;
        if (k >= 4 && nm[k-4] == '.' &&
            (nm[k-3]|32) == 'w' && (nm[k-2]|32) == 'a' && (nm[k-1]|32) == 'v') {
            for (int j = 0; j <= k; j++) wavs[nwav][j] = nm[j];
            nwav++;
        }
    }
}

static void draw(int playing) {
    sys_clear();
    sys_setcolor(4); print("  == Jukebox ==   real .WAV playback (AC'97)\n\n"); sys_setcolor(0);
    if (nwav == 0) print("  (no .WAV files on disk)\n");
    for (int i = 0; i < nwav; i++) {
        char line[40]; int p = 0;
        line[p++] = ' '; line[p++] = ' ';
        line[p++] = (char)('1' + i); line[p++] = ')'; line[p++] = ' ';
        for (int j = 0; wavs[i][j] && p < 34; j++) line[p++] = wavs[i][j];
        if (i == playing) { line[p++] = ' '; line[p++] = '<'; line[p++] = '<'; }
        line[p] = 0;
        if (i == playing) sys_setcolor(3);           /* green for the playing track */
        print(line); print("\n");
        if (i == playing) sys_setcolor(0);
    }
    print("\n  [1-9] play   [s] stop   [q] quit (music keeps playing)\n");
}

int main(void) {
    scan_wavs();
    int playing = -1;
    draw(playing);
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(30); continue; }
        if (k == 'q' || k == 27) return 0;           /* leaving doesn't stop the music */
        else if (k == 's') { sys_audiostop(); playing = -1; draw(playing); }
        else if (k >= '1' && k <= '9') {
            int i = k - '1';
            if (i < nwav && sys_playbg(wavs[i]) == 0) { playing = i; draw(playing); }
        }
    }
}
