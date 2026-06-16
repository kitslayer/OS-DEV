/*
 * wordle.c — the Wordle word game, a userspace program.
 *
 * Guess a hidden five-letter word in six tries. Type letters, Enter submits a
 * full row, Backspace deletes. After each guess each letter is coloured:
 * green = right letter in the right spot, yellow = the letter is in the word
 * but elsewhere, grey = not in the word (the standard rules, duplicate letters
 * handled correctly). The answer is picked from a baked word list with a
 * clock-seeded PRNG. ESC quits; Enter starts a new round once a game ends.
 */
#include "ulib.h"

/* ~110 common five-letter words; the answer is one of these. */
static const char *words[] = {
    "apple","beach","brain","bread","brick","brush","chair","chest","chord","click",
    "clock","cloud","crane","crisp","crown","dance","dream","drink","earth","flame",
    "flask","fleet","flute","frost","ghost","glass","globe","grape","grass","green",
    "heart","honey","house","ivory","joker","juice","knife","label","lemon","light",
    "lunar","maple","march","medal","money","mouse","music","night","noble","ocean",
    "olive","paint","panel","pearl","phone","piano","pilot","plant","plate","poker",
    "pride","prime","prize","proof","proud","queen","quiet","quill","quilt","radio",
    "raven","river","robot","royal","sugar","saint","scale","scout","shark","sheep",
    "shine","shore","sight","silk", "skate","sleep","slice","smile","smoke","snail",
    "snake","solar","sound","space","spark","spice","spine","spoon","stone","storm",
    "story","sweet","table","tiger","toast","tower","train","treat","trend","trout",
    "tulip","vivid","vocal","wagon","whale","wheat","witch","world","zebra",
};
#define NW ((int)(sizeof(words)/sizeof(words[0])))

#define ROWS 6
#define LEN  5

static char guesses[ROWS][LEN];   /* submitted guesses (uppercase) */
static int  nrows;                /* number submitted */
static char cur[LEN];             /* the row being typed */
static int  curn;
static const char *answer;        /* lowercase */
static int  state;                /* 0 playing, 1 won, 2 lost */
static const char *msg;           /* transient status line */

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static char up(char c)  { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static char low(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Colour each letter of guess `g` against the answer (Wordle rules with
 * duplicate handling): out[i] = 0 green, 3 yellow, 8 grey (palette indices). */
static void score(const char *g, unsigned char *out) {
    char ans[LEN]; int used[LEN];
    for (int i = 0; i < LEN; i++) { ans[i] = answer[i]; used[i] = 0; out[i] = 8; }
    for (int i = 0; i < LEN; i++)                            /* pass 1: greens */
        if (low(g[i]) == ans[i]) { out[i] = 0; used[i] = 1; }
    for (int i = 0; i < LEN; i++) {                          /* pass 2: yellows from leftovers */
        if (out[i] == 0) continue;
        for (int j = 0; j < LEN; j++)
            if (!used[j] && low(g[i]) == ans[j]) { out[i] = 3; used[j] = 1; break; }
    }
}

static void newgame(void) {
    answer = words[rnd() % NW];
    nrows = 0; curn = 0; state = 0; msg = "type a word, Enter submits";
}

static void render(void) {
    sys_clear();
    sys_setcolor(1); print(" WORDLE");
    sys_setcolor(8);
    if (state == 0) {
        char hd[5] = { ' ', (char)('1' + (nrows < ROWS ? nrows : ROWS - 1)), '/', '0' + ROWS, 0 };
        print("  guess"); print(hd);
    }
    print("\n\n");

    for (int r = 0; r < ROWS; r++) {
        print("    ");
        if (r < nrows) {                                    /* a submitted, scored row */
            unsigned char col[LEN]; score(guesses[r], col);
            for (int i = 0; i < LEN; i++) {
                sys_setcolor(col[i]);
                char cb[3] = { guesses[r][i], ' ', 0 }; print(cb);
            }
        } else if (r == nrows && state == 0) {              /* the row being typed */
            for (int i = 0; i < LEN; i++) {
                sys_setcolor(1);
                char cb[3] = { i < curn ? cur[i] : '_', ' ', 0 }; print(cb);
            }
        } else {                                            /* empty future row */
            sys_setcolor(8);
            for (int i = 0; i < LEN; i++) print(". ");
        }
        print("\n");
    }
    print("\n");
    if (state == 1) { sys_setcolor(0); print(" you got it! "); }
    else if (state == 2) {
        sys_setcolor(2); print(" the word was ");
        char w[LEN + 1]; for (int i = 0; i < LEN; i++) w[i] = up(answer[i]); w[LEN] = 0;
        print(w);
    } else { sys_setcolor(8); print(" "); print(msg); }
    if (state != 0) { sys_setcolor(8); print("\n Enter=new game  ESC=quit"); }
    sys_setcolor(0);
}

static void submit(void) {
    if (curn < LEN) { msg = "not enough letters"; return; }
    for (int i = 0; i < LEN; i++) guesses[nrows][i] = up(cur[i]);
    nrows++; curn = 0; msg = "type a word, Enter submits";
    int win = 1;
    for (int i = 0; i < LEN; i++) if (low(guesses[nrows - 1][i]) != answer[i]) win = 0;
    if (win) state = 1;
    else if (nrows >= ROWS) state = 2;
}

int main(void) {
    char tb[40]; long tn = sys_time(tb, sizeof(tb));        /* clock-seeded PRNG */
    rng = 0x2545F491u;
    for (long i = 0; i < tn; i++) rng = rng * 31u + (unsigned char)tb[i];
    if (!rng) rng = 12345u;
    rnd(); rnd();
    newgame();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(15); continue; }
        if (k == 27) return 0;                              /* ESC: quit */
        if (state != 0) {                                   /* game over: Enter restarts */
            if (k == '\n' || k == '\r') { newgame(); render(); }
            continue;
        }
        if (k == '\n' || k == '\r') submit();
        else if (k == 8 || k == 127) { if (curn > 0) curn--; }   /* Backspace */
        else if (((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z')) && curn < LEN)
            cur[curn++] = up((char)k);
        else continue;
        render();
    }
}
