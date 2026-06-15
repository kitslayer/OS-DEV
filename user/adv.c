/*
 * adv.c — a small text adventure, a userspace program.
 *
 * A parser-driven dungeon crawl: read a command line (readline), act on it, print
 * the result, repeat — turn-based, terminal-style (unlike the real-time games).
 * Find the torch, light the cave, and grab the gold to win. Commands: n/s/e/w to
 * move, look, take, i (inventory), help, quit. Runs ring-3.
 */
#include "ulib.h"

/* items */
enum { I_KEY, I_SWORD, I_TORCH, I_GOLD, NITEMS };
static const char *ITEM[NITEMS] = { "key", "sword", "torch", "gold" };

/* rooms */
enum { R_ENTRANCE, R_HALL, R_LIBRARY, R_ARMORY, R_CELLAR, R_CAVE, NROOMS };
static const char *RNAME[NROOMS] = { "Entrance", "Hall", "Library", "Armory", "Cellar", "Cave" };
static const char *RDESC[NROOMS] = {
    "A cold stone entrance. A passage leads east.",
    "A torchlit hall. Ways lead west, north, east, and down.",
    "Dusty shelves line a quiet library. Stairs lead down.",
    "An old armory, racks mostly bare. A door leads west.",
    "A damp cellar. A dark tunnel yawns to the east; stairs lead up.",
    "Pitch black. You can feel a tunnel back to the west.",
};
/* exits[r][dir], dir: 0=N 1=S 2=E 3=W; -1 = no exit */
static const int EXIT[NROOMS][4] = {
    /* Entrance */ { -1, -1, R_HALL, -1 },
    /* Hall     */ { R_LIBRARY, R_CELLAR, R_ARMORY, R_ENTRANCE },
    /* Library  */ { -1, R_HALL, -1, -1 },
    /* Armory   */ { -1, -1, -1, R_HALL },
    /* Cellar   */ { R_HALL, -1, R_CAVE, -1 },
    /* Cave     */ { -1, -1, -1, R_CELLAR },
};

static int room_item[NROOMS];   /* item in each room, or -1 */
static int have[NITEMS];        /* 1 if carried */
static int here;                /* current room */
static int won;

static int lit_cave(void) { return have[I_TORCH]; }    /* the torch lights the cave */

static void describe(void) {
    print("\n");
    sys_setcolor(4); print(RNAME[here]); sys_setcolor(0); print("\n");   /* room name: cyan */
    if (here == R_CAVE && !lit_cave()) {
        print("It is too dark to see anything. You need a light.\n");
        return;
    }
    if (here == R_CAVE) print("By your torchlight, a heap of GOLD glitters!\n");
    else                print(RDESC[here]);
    if (here != R_CAVE) print("\n");
    int it = room_item[here];
    if (it >= 0 && !(here == R_CAVE && !lit_cave())) {
        print("You see a "); sys_setcolor(3); print(ITEM[it]); sys_setcolor(0); print(" here.\n");
    }
}

/* one direction word -> 0..3, or -1 */
static int dirof(const char *s) {
    if (streq(s, "n") || streq(s, "north")) return 0;
    if (streq(s, "s") || streq(s, "south")) return 1;
    if (streq(s, "e") || streq(s, "east"))  return 2;
    if (streq(s, "w") || streq(s, "west"))  return 3;
    return -1;
}

static void take(void) {
    if (here == R_CAVE && !lit_cave()) { print("You grope in the dark and find nothing.\n"); return; }
    int it = room_item[here];
    if (it < 0) { print("There is nothing here to take.\n"); return; }
    have[it] = 1; room_item[here] = -1;
    print("You take the "); sys_setcolor(3); print(ITEM[it]); sys_setcolor(0); print(".\n");
    if (it == I_GOLD) {
        won = 1;
        sys_setcolor(9); print("\n*** You hoist the gold and stride out, rich. You win! ***\n"); sys_setcolor(0);
        print("(type 'quit', or 'look' to linger)\n");
    }
}

static void inventory(void) {
    int n = 0;
    print("You are carrying:");
    for (int i = 0; i < NITEMS; i++) if (have[i]) { print(n ? ", " : " "); sys_setcolor(3); print(ITEM[i]); sys_setcolor(0); n++; }
    print(n ? "\n" : " nothing.\n");
}

static void go(int d) {
    int nx = EXIT[here][d];
    if (nx < 0) { print("You can't go that way.\n"); return; }
    here = nx;
    describe();
}

int main(void) {
    for (int i = 0; i < NROOMS; i++) room_item[i] = -1;
    room_item[R_LIBRARY] = I_KEY;
    room_item[R_ARMORY]  = I_SWORD;
    room_item[R_CELLAR]  = I_TORCH;
    room_item[R_CAVE]    = I_GOLD;
    here = R_ENTRANCE; won = 0;

    sys_setcolor(4); print("  == The Cellar of Gold ==\n"); sys_setcolor(0);
    print("A tiny text adventure. Type 'help' for commands.\n");
    describe();

    char cmd[40];
    for (;;) {
        sys_setcolor(9); print("\n> "); sys_setcolor(0);
        int n = readline(cmd, sizeof(cmd));
        if (n <= 0) continue;
        int d = dirof(cmd);
        if (d >= 0) go(d);
        else if (streq(cmd, "look") || streq(cmd, "l")) describe();
        else if (streq(cmd, "take") || streq(cmd, "get") || streq(cmd, "t")) take();
        else if (streq(cmd, "i") || streq(cmd, "inv") || streq(cmd, "inventory")) inventory();
        else if (streq(cmd, "help") || streq(cmd, "h") || streq(cmd, "?"))
            print("commands: n s e w, look, take, i (inventory), help, quit\n");
        else if (streq(cmd, "quit") || streq(cmd, "q")) return 0;
        else print("I don't understand that.\n");
    }
}
