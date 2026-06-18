/*
 * mastermind.c — the code-breaking game.
 *
 * The computer picks a secret code of 4 pegs, each a digit 1-6 (repeats
 * allowed). You have 10 guesses. After each, you're scored: '#' = a peg of the
 * right colour in the right place, 'o' = a right colour in the wrong place.
 * Crack the code before you run out of rows.
 *
 * Type digits 1-6 to build a guess, Backspace to erase, Enter to submit.
 */
#include "ulib.h"

#define LEN  4
#define ROWS 10

static int secret[LEN];
static int guesses[ROWS][LEN];
static int blacks[ROWS], whites[ROWS];
static int nrows;                   /* completed guesses */
static int cur[LEN], ncur;          /* current input */
static int over, win;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void putn(int n) { char s[2]; s[0] = (char)('0' + n); s[1] = 0; print(s); }

static void newgame(void) {
    for (int i = 0; i < LEN; i++) secret[i] = 1 + (int)(rnd() % 6);
    nrows = ncur = over = win = 0;
}

static void score(const int *g, int *bl, int *wh) {
    int sc[7] = {0}, gc[7] = {0}, b = 0;
    for (int i = 0; i < LEN; i++) {
        if (g[i] == secret[i]) b++;
        else { sc[secret[i]]++; gc[g[i]]++; }
    }
    int m = 0;
    for (int c = 1; c <= 6; c++) m += sc[c] < gc[c] ? sc[c] : gc[c];
    *bl = b; *wh = m;
}

static void submit(void) {
    if (ncur < LEN) return;
    for (int i = 0; i < LEN; i++) guesses[nrows][i] = cur[i];
    int b, w; score(cur, &b, &w);
    blacks[nrows] = b; whites[nrows] = w; nrows++;
    ncur = 0;
    if (b == LEN) { over = win = 1; sys_beep(880,120); sys_beep(1320,140); }
    else if (nrows >= ROWS) { over = 1; sys_beep(196, 250); }
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("\n  Mastermind"); sys_setcolor(0); print("   crack the 4-peg 1-6 code\n\n");
    for (int r = 0; r < ROWS; r++) {
        print("   ");
        if (r < nrows) {
            for (int i = 0; i < LEN; i++) { sys_setcolor(8 + guesses[r][i]); print(" "); putn(guesses[r][i]); }
            sys_setcolor(0); print("   ");
            sys_setcolor(2);  for (int i = 0; i < blacks[r]; i++) print("#");
            sys_setcolor(15); for (int i = 0; i < whites[r]; i++) print("o");
            sys_setcolor(8);  for (int i = 0; i < LEN - blacks[r] - whites[r]; i++) print(".");
        } else if (r == nrows && !over) {
            for (int i = 0; i < LEN; i++) {
                if (i < ncur) { sys_setcolor(8 + cur[i]); print(" "); putn(cur[i]); }
                else { sys_setcolor(8); print(" _"); }
            }
            sys_setcolor(7); print("   <-");
        } else {
            sys_setcolor(8); for (int i = 0; i < LEN; i++) print(" -");
        }
        sys_setcolor(0); print("\n");
    }
    print("\n  ");
    if (win) { sys_setcolor(10); print("Cracked it in "); putn(nrows); print("!  (n = new)"); }
    else if (over) {
        sys_setcolor(2); print("Out of guesses. Code: ");
        for (int i = 0; i < LEN; i++) { sys_setcolor(8 + secret[i]); putn(secret[i]); print(" "); }
    } else { sys_setcolor(0); print("type 1-6  bksp erase  enter submit"); }
    sys_setcolor(0); print("\n  n new   q quit\n");
}

int main(void) {
    rng = (unsigned)sys_uptime_ms() | 1u;
    newgame();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (k == 'n' || k == 'N') { newgame(); render(); continue; }
        if (over) continue;
        if (k >= '1' && k <= '6') { if (ncur < LEN) { cur[ncur++] = k - '0'; render(); } }
        else if (k == 8 || k == 127) { if (ncur > 0) { ncur--; render(); } }   /* backspace */
        else if (k == '\n' || k == '\r') { submit(); render(); }
    }
    return 0;
}
