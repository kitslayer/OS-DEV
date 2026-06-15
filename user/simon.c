/*
 * simon.c — "Simon": an audio-visual memory game.
 *
 * A genre the app suite lacked: short-term memory + sound. The machine flashes
 * a growing sequence of four coloured pads, each with its own tone (via the PC
 * speaker); you repeat it with keys 1-4. Every round adds one step. Miss one and
 * the game's over — your longest run persists in SIMON.HI. A small showcase of
 * the colour palette and sys_beep together.
 */
#include "ulib.h"

#define MAXSEQ 64

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int seq[MAXSEQ];
static int seqlen;            /* current sequence length == round number */
static int inpos;             /* how many the player has correctly echoed this round */
static int state;             /* 0 = player's turn (INPUT), 1 = game over */
static int best;              /* longest run, persisted to SIMON.HI */

static const int   TONE[4] = { 330, 392, 494, 587 };          /* E4 G4 B4 D5 */
static const int   COL[4]  = { 2, 1, 4, 3 };                  /* red, green, cyan, yellow */
static const char *NAME[4] = { "RED", "GRN", "CYN", "YEL" };

static int itoa_b(int v, char *out) {
    char t[12]; int i = 0, n = v < 0 ? 0 : v;
    if (n == 0) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    int j = 0; while (i) out[j++] = t[--i];
    out[j] = 0; return j;
}
static void printnum(int v) { char b[12]; itoa_b(v, b); print(b); }

static void load_best(void) {
    char b[16]; long n = sys_readfile("SIMON.HI", b, sizeof(b) - 1);
    best = 0;
    for (long i = 0; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') break;
        best = best * 10 + (b[i] - '0');
        if (best > 100000) { best = 100000; break; }   /* clamp a corrupt file */
    }
}
static void save_best(void) { char b[12]; int n = itoa_b(best, b); sys_writefile("SIMON.HI", b, (unsigned long)n); }

static void render(int lit, const char *msg) {
    sys_clear();
    sys_setcolor(4); print("\n  Simon"); sys_setcolor(0);
    print("     round "); printnum(seqlen);
    print("     best "); sys_setcolor(3); printnum(best); sys_setcolor(0);
    print("\n\n");
    for (int i = 0; i < 4; i++) {
        sys_setcolor(i == lit ? COL[i] : 8);              /* lit pad in colour, others grey */
        char row[20]; int p = 0;
        row[p++] = ' '; row[p++] = ' '; row[p++] = ' ';
        row[p++] = (char)('1' + i); row[p++] = ':'; row[p++] = ' '; row[p++] = '[';
        for (const char *s = NAME[i]; *s; s++) row[p++] = *s;
        row[p++] = ']'; row[p] = 0;
        print(row); print("\n");
    }
    sys_setcolor(0);
    print("\n  "); print(msg);
    print("\n  q quit\n");
}

static void flash_sequence(void) {
    render(-1, "watch...");
    sys_sleep(500);
    for (int i = 0; i < seqlen; i++) {
        render(seq[i], "watch...");
        sys_beep(TONE[seq[i]], 350);                       /* tone plays while the pad is lit */
        render(-1, "watch...");
        sys_sleep(180);
    }
    render(-1, "your turn! press 1-4");
}

static void new_game(void) {
    seqlen = 1; seq[0] = (int)(rnd() & 3); inpos = 0; state = 0;
    flash_sequence();
}

int main(void) {
    char tb[40]; long tn = sys_time(tb, sizeof(tb));
    rng = 0x2545F491u;
    for (long i = 0; i < tn; i++) rng = rng * 31u + (unsigned char)tb[i];
    if (!rng) rng = 12345u;

    load_best();
    new_game();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') break;
        if (state == 1) { if (k == ' ') new_game(); continue; }   /* game over: SPACE restarts (NOT "any key": a stray Enter would otherwise dismiss it instantly) */
        if (k >= '1' && k <= '4') {
            int pad = k - '1';
            render(pad, "your turn! press 1-4");            /* light + sound the pressed pad */
            sys_beep(TONE[pad], 180);
            if (pad == seq[inpos]) {
                inpos++;
                if (inpos == seqlen) {                       /* completed the round */
                    if (seqlen > best) { best = seqlen; save_best(); }
                    if (seqlen < MAXSEQ) {
                        seq[seqlen] = (int)(rnd() & 3);
                        seqlen++; inpos = 0;
                        flash_sequence();
                    } else { state = 1; render(-1, "MAX! you win!  SPACE = again"); }
                } else {
                    render(-1, "your turn! press 1-4");
                }
            } else {
                state = 1;
                sys_beep(150, 400);
                render(-1, "WRONG! game over.  SPACE = again");
            }
        }
    }
    return 0;
}
