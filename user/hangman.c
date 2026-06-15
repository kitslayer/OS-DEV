/*
 * hangman.c — the word-guessing game, a userspace program.
 *
 * Guess the hidden word a letter at a time; each wrong letter draws another part
 * of the gallows figure (six strikes and it's over). Letters reveal in place; the
 * answer is shown on a loss. n starts a new word, q quits. A word-game genre to
 * sit beside the arcade and puzzle games. Runs ring-3.
 */
#include "ulib.h"

static const char *WORDS[] = {
    "planet","crystal","desktop","keyboard","compiler","network","penguin","diamond",
    "octopus","rainbow","library","machine","quantum","gravity","voyager","kernel",
    "pixel","syntax","cascade","triangle",
};
#define NW ((int)(sizeof(WORDS) / sizeof(WORDS[0])))

static unsigned rng = 5381u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int wi;            /* current word index */
static int guessed[26];
static int wrong;
static int over;          /* 0 playing, 1 won, 2 lost */

static const char *word(void) { return WORDS[wi]; }

static int all_revealed(void) {
    const char *w = word();
    for (int i = 0; w[i]; i++) if (!guessed[w[i] - 'a']) return 0;
    return 1;
}
static void newgame(void) { wi = (int)(rnd() % NW); for (int i = 0; i < 26; i++) guessed[i] = 0; wrong = 0; over = 0; }

static void putc1(char c) { char b[2] = { c, 0 }; print(b); }

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("  Hangman\n\n");

    /* the gallows + figure (built up by wrong-count) */
    sys_setcolor(over == 2 ? 2 : 8);
    print("   +---+\n");
    print("   |   "); sys_setcolor(over == 2 ? 2 : 3); putc1(wrong >= 1 ? 'O' : ' '); print("\n");
    sys_setcolor(over == 2 ? 2 : 8);
    print("   |  "); putc1(wrong >= 3 ? '/' : ' '); putc1(wrong >= 2 ? '|' : ' '); putc1(wrong >= 4 ? '\\' : ' '); print("\n");
    print("   |  "); putc1(wrong >= 5 ? '/' : ' '); print(" "); putc1(wrong >= 6 ? '\\' : ' '); print("\n");
    print("   |\n");
    print("  =+==\n\n");

    /* the word: revealed letters (uppercase), or _ ; whole word shown when over */
    sys_setcolor(over == 1 ? 9 : (over == 2 ? 2 : 3));
    const char *w = word();
    print("  ");
    for (int i = 0; w[i]; i++) {
        int show = over || guessed[w[i] - 'a'];
        putc1(show ? (char)(w[i] - 32) : '_'); putc1(' ');
    }
    print("\n\n");

    /* guessed letters + status */
    sys_setcolor(8); print("  tried: ");
    for (int i = 0; i < 26; i++) if (guessed[i]) { sys_setcolor(8); putc1((char)('a' + i)); putc1(' '); }
    print("\n");
    char m[8]; int p = 0; m[p++] = '0' + (6 - wrong) / 10; m[p++] = '0' + (6 - wrong) % 10; m[p] = 0;
    sys_setcolor(8); print("  guesses left: "); print(m + (m[0] == '0' ? 1 : 0)); print("\n\n");

    if (over == 1) { sys_setcolor(9); print("  ** you got it! **  n=new q=quit\n"); }
    else if (over == 2) { sys_setcolor(2); print("  ** out of guesses **  n=new q=quit\n"); }
    else { sys_setcolor(8); print("  type a letter   n=new  q=quit\n"); }
    sys_setcolor(0);
}

int main(void) {
    char tb[24]; sys_time(tb, sizeof(tb));
    for (int i = 0; tb[i]; i++) rng = rng * 31 + (unsigned char)tb[i];
    newgame();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 27) return 0;
        if (k == 'n') { newgame(); render(); continue; }
        if (over) continue;
        if (k >= 'A' && k <= 'Z') k += 32;
        if (k < 'a' || k > 'z') continue;
        if (guessed[k - 'a']) continue;            /* already tried */
        guessed[k - 'a'] = 1;
        const char *w = word(); int hit = 0;
        for (int i = 0; w[i]; i++) if (w[i] == k) hit = 1;
        if (!hit) { if (++wrong >= 6) over = 2; }
        else if (all_revealed()) over = 1;
        render();
    }
}
