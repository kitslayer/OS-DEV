/*
 * chess.c — chess against the computer.
 *
 * Full legal-move chess: every piece moves by the real rules, you may not leave
 * your own king in check, pawns promote to a queen on the last rank, and the
 * game ends on checkmate or stalemate. You play White (uppercase, at the
 * bottom); the computer plays Black with a small alpha-beta search over
 * material.
 *
 * Move the cursor with the arrows, press Space on one of your pieces to pick it
 * up, then Space on a destination to move (Space on another of your pieces
 * re-picks). r starts a new game, q quits.
 *
 * Castling is supported (rights tracked, the king may not castle out of, through,
 * or into check); en-passant is not modelled (rare).
 */
#include "ulib.h"

/* board[64], index = rank-row*8 + file-col. row 0 = top (Black's back rank,
 * shown as "8"); row 7 = bottom (White's back rank, "1"). White = uppercase
 * and moves UP the board (row decreasing); Black = lowercase, moves DOWN. */
static char bd[64];
static int  cr, cc;            /* cursor row/col */
static int  sel;               /* selected square index, or -1 */
static int  over;              /* game ended */
static int  crights;           /* castling rights bitmask: 1=WK 2=WQ 4=BK 8=BQ */
static int  lfrom = -1, lto = -1;   /* last move's from/to squares (highlighted) */
static const char *msg;

typedef struct { unsigned char from, to, promo; } Move;

#define INF 1000000
#define MATE 100000

static int  is_w(char p) { return p >= 'A' && p <= 'Z'; }
static int  is_b(char p) { return p >= 'a' && p <= 'z'; }
static int  empty(char p) { return p == '.'; }
static int  side_of(char p) { return is_w(p) ? 1 : (is_b(p) ? 0 : -1); }
static char up(char p) { return (p >= 'a' && p <= 'z') ? (char)(p - 32) : p; }

static void reset(void) {
    static const char *back = "rnbqkbnr";
    for (int i = 0; i < 64; i++) bd[i] = '.';
    for (int c = 0; c < 8; c++) {
        bd[0*8 + c] = back[c];           /* black back rank (row 0) */
        bd[1*8 + c] = 'p';               /* black pawns */
        bd[6*8 + c] = 'P';               /* white pawns */
        bd[7*8 + c] = (char)(back[c] - 32); /* white back rank, uppercase */
    }
    crights = 15; cr = 6; cc = 4; sel = -1; over = 0; lfrom = lto = -1; msg = "Your move (White).";
}

static int pval(char p) {
    switch (up(p)) {
        case 'P': return 100; case 'N': return 320; case 'B': return 330;
        case 'R': return 500; case 'Q': return 900; case 'K': return 20000;
    }
    return 0;
}

/* Is square (r,c) attacked by side `byw` (1=white,0=black)? Probe outward from
 * the square: knights, king, pawns, and sliders along rays. */
static int attacked(int r, int c, int byw) {
    static const int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (int i = 0; i < 8; i++) {
        int rr = r + kn[i][0], cc2 = c + kn[i][1];
        if (rr < 0 || rr > 7 || cc2 < 0 || cc2 > 7) continue;
        char p = bd[rr*8 + cc2];
        if (up(p) == 'N' && side_of(p) == byw) return 1;
    }
    for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
        if (!dr && !dc) continue;
        int rr = r + dr, cc2 = c + dc;
        if (rr < 0 || rr > 7 || cc2 < 0 || cc2 > 7) continue;
        char p = bd[rr*8 + cc2];
        if (up(p) == 'K' && side_of(p) == byw) return 1;
    }
    /* pawns: a white pawn attacking (r,c) sits at (r+1,c±1); black at (r-1,c±1) */
    int pr = byw ? r + 1 : r - 1;
    if (pr >= 0 && pr <= 7) {
        for (int dc = -1; dc <= 1; dc += 2) {
            int cc2 = c + dc;
            if (cc2 < 0 || cc2 > 7) continue;
            char p = bd[pr*8 + cc2];
            if (up(p) == 'P' && side_of(p) == byw) return 1;
        }
    }
    /* sliders: bishops/queens on diagonals, rooks/queens on files/ranks */
    static const int diag[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    static const int orth[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int i = 0; i < 4; i++) {
        int rr = r + diag[i][0], cc2 = c + diag[i][1];
        while (rr >= 0 && rr <= 7 && cc2 >= 0 && cc2 <= 7) {
            char p = bd[rr*8 + cc2];
            if (!empty(p)) { if (side_of(p) == byw && (up(p) == 'B' || up(p) == 'Q')) return 1; break; }
            rr += diag[i][0]; cc2 += diag[i][1];
        }
    }
    for (int i = 0; i < 4; i++) {
        int rr = r + orth[i][0], cc2 = c + orth[i][1];
        while (rr >= 0 && rr <= 7 && cc2 >= 0 && cc2 <= 7) {
            char p = bd[rr*8 + cc2];
            if (!empty(p)) { if (side_of(p) == byw && (up(p) == 'R' || up(p) == 'Q')) return 1; break; }
            rr += orth[i][0]; cc2 += orth[i][1];
        }
    }
    return 0;
}

static int king_sq(int white) {
    char k = white ? 'K' : 'k';
    for (int i = 0; i < 64; i++) if (bd[i] == k) return i;
    return -1;
}
static int in_check(int white) {
    int ks = king_sq(white);
    if (ks < 0) return 1;
    return attacked(ks / 8, ks % 8, !white);
}

/* Pseudo-legal moves for `white` into list[], returning the count. Pawn
 * promotions are emitted as a single auto-queen move. */
static int gen_pseudo(int white, Move *list) {
    int n = 0;
    static const int kn[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    static const int diag[4][2] = {{-1,-1},{-1,1},{1,-1},{1,1}};
    static const int orth[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
        char p = bd[r*8 + c];
        if (empty(p) || side_of(p) != white) continue;
        char P = up(p);
        if (P == 'P') {
            int dir = white ? -1 : 1, start = white ? 6 : 1, last = white ? 0 : 7;
            int r1 = r + dir;
            if (r1 >= 0 && r1 <= 7 && empty(bd[r1*8 + c])) {           /* forward 1 */
                unsigned char promo = (r1 == last) ? (white ? 'Q' : 'q') : 0;
                list[n++] = (Move){ (unsigned char)(r*8+c), (unsigned char)(r1*8+c), promo };
                if (r == start && empty(bd[(r+2*dir)*8 + c]))           /* forward 2 */
                    list[n++] = (Move){ (unsigned char)(r*8+c), (unsigned char)((r+2*dir)*8+c), 0 };
            }
            for (int dc = -1; dc <= 1; dc += 2) {                       /* captures */
                int cc2 = c + dc;
                if (r1 < 0 || r1 > 7 || cc2 < 0 || cc2 > 7) continue;
                char t = bd[r1*8 + cc2];
                if (!empty(t) && side_of(t) != white) {
                    unsigned char promo = (r1 == last) ? (white ? 'Q' : 'q') : 0;
                    list[n++] = (Move){ (unsigned char)(r*8+c), (unsigned char)(r1*8+cc2), promo };
                }
            }
        } else if (P == 'N') {
            for (int i = 0; i < 8; i++) {
                int rr = r + kn[i][0], cc2 = c + kn[i][1];
                if (rr < 0 || rr > 7 || cc2 < 0 || cc2 > 7) continue;
                char t = bd[rr*8 + cc2];
                if (empty(t) || side_of(t) != white)
                    list[n++] = (Move){ (unsigned char)(r*8+c), (unsigned char)(rr*8+cc2), 0 };
            }
        } else if (P == 'K') {
            for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) {
                if (!dr && !dc) continue;
                int rr = r + dr, cc2 = c + dc;
                if (rr < 0 || rr > 7 || cc2 < 0 || cc2 > 7) continue;
                char t = bd[rr*8 + cc2];
                if (empty(t) || side_of(t) != white)
                    list[n++] = (Move){ (unsigned char)(r*8+c), (unsigned char)(rr*8+cc2), 0 };
            }
            if (c == 4 && ((white && r == 7) || (!white && r == 0))) {   /* castling */
                int wk = white ? 1 : 4, wq = white ? 2 : 8;
                if ((crights & wk) && empty(bd[r*8+5]) && empty(bd[r*8+6]) && up(bd[r*8+7]) == 'R'
                    && !attacked(r,4,!white) && !attacked(r,5,!white) && !attacked(r,6,!white))
                    list[n++] = (Move){ (unsigned char)(r*8+4), (unsigned char)(r*8+6), 0 };
                if ((crights & wq) && empty(bd[r*8+1]) && empty(bd[r*8+2]) && empty(bd[r*8+3]) && up(bd[r*8+0]) == 'R'
                    && !attacked(r,4,!white) && !attacked(r,3,!white) && !attacked(r,2,!white))
                    list[n++] = (Move){ (unsigned char)(r*8+4), (unsigned char)(r*8+2), 0 };
            }
        } else {   /* sliders: B, R, Q */
            const int (*dirs)[2]; int nd;
            if (P == 'B') { dirs = diag; nd = 4; }
            else if (P == 'R') { dirs = orth; nd = 4; }
            else { /* Q */ static int qd[8][2]; for (int i=0;i<4;i++){qd[i][0]=diag[i][0];qd[i][1]=diag[i][1];qd[i+4][0]=orth[i][0];qd[i+4][1]=orth[i][1];} dirs = qd; nd = 8; }
            for (int i = 0; i < nd; i++) {
                int rr = r + dirs[i][0], cc2 = c + dirs[i][1];
                while (rr >= 0 && rr <= 7 && cc2 >= 0 && cc2 <= 7) {
                    char t = bd[rr*8 + cc2];
                    if (empty(t)) list[n++] = (Move){ (unsigned char)(r*8+c), (unsigned char)(rr*8+cc2), 0 };
                    else { if (side_of(t) != white) list[n++] = (Move){ (unsigned char)(r*8+c), (unsigned char)(rr*8+cc2), 0 }; break; }
                    rr += dirs[i][0]; cc2 += dirs[i][1];
                }
            }
        }
    }
    return n;
}

static void clr_rights_sq(int sq) {     /* a king/rook left or was captured on sq -> drop that right */
    if (sq == 60) crights &= ~(1 | 2);  /* e1: White king */
    if (sq == 4)  crights &= ~(4 | 8);  /* e8: Black king */
    if (sq == 63) crights &= ~1;        /* h1 rook -> White kingside  */
    if (sq == 56) crights &= ~2;        /* a1 rook -> White queenside */
    if (sq == 7)  crights &= ~4;        /* h8 rook -> Black kingside  */
    if (sq == 0)  crights &= ~8;        /* a8 rook -> Black queenside */
}

/* backup is 65 bytes: bytes 0..63 = board, byte 64 = castling rights. */
static void apply(Move m, char *backup) {
    for (int i = 0; i < 64; i++) backup[i] = bd[i];
    backup[64] = (char)crights;
    char p = bd[m.from];
    if (up(p) == 'K') {                  /* a two-file king step is a castle: move the rook too */
        int r = m.from / 8, fc = m.from % 8, tc = m.to % 8;
        if (tc - fc == 2) { bd[r*8+5] = bd[r*8+7]; bd[r*8+7] = '.'; }   /* kingside  h->f */
        if (fc - tc == 2) { bd[r*8+3] = bd[r*8+0]; bd[r*8+0] = '.'; }   /* queenside a->d */
    }
    bd[m.to] = m.promo ? (char)m.promo : p;
    bd[m.from] = '.';
    clr_rights_sq(m.from);
    clr_rights_sq(m.to);
}
static void undo(char *backup) {
    for (int i = 0; i < 64; i++) bd[i] = backup[i];
    crights = (unsigned char)backup[64];
}

/* Legal moves = pseudo-legal that don't leave our own king in check. */
static int gen_legal(int white, Move *out) {
    Move ps[256]; int np = gen_pseudo(white, ps), n = 0;
    char bak[65];
    for (int i = 0; i < np; i++) {
        apply(ps[i], bak);
        if (!in_check(white)) out[n++] = ps[i];
        undo(bak);
    }
    return n;
}

/* Piece-square tables (Michniewski's simplified set) in this file's board
 * layout: row 0 = top = rank 8, so a White piece reads pst[idx] directly and a
 * Black piece reads the vertically-mirrored square. They nudge the otherwise
 * material-only search toward sensible squares — centre control, development,
 * advanced pawns, a tucked-away king — without ever outweighing a real capture. */
static const int PST_P[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0 };
static const int PST_N[64] = {
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50 };
static const int PST_B[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -20,-10,-10,-10,-10,-10,-10,-20 };
static const int PST_R[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0 };
static const int PST_Q[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5,  5,  5,  5,  0,-10,
    -5,  0,  5,  5,  5,  5,  0, -5,
     0,  0,  5,  5,  5,  5,  0, -5,
   -10,  5,  5,  5,  5,  5,  0,-10,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20 };
static const int PST_K[64] = {
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -10,-20,-20,-20,-20,-20,-20,-10,
    20, 20,  0,  0,  0,  0, 20, 20,
    20, 30, 10,  0,  0, 10, 30, 20 };

static const int *pst_for(char P) {
    switch (P) {
        case 'P': return PST_P; case 'N': return PST_N; case 'B': return PST_B;
        case 'R': return PST_R; case 'Q': return PST_Q; case 'K': return PST_K;
    }
    return 0;
}

static int evaluate(void) {           /* + good for White, - for Black */
    int s = 0;
    for (int i = 0; i < 64; i++) {
        char p = bd[i];
        if (empty(p)) continue;
        const int *t = pst_for(up(p));
        int idx = is_w(p) ? i : ((7 - i/8) * 8 + i % 8);   /* Black reads the mirrored square */
        int v = pval(p) + (t ? t[idx] : 0);
        s += is_w(p) ? v : -v;
    }
    return s;
}

/* Negamax with alpha-beta. Returns score from the perspective of `white`. */
static int negamax(int depth, int alpha, int beta, int white) {
    if (depth == 0) return white ? evaluate() : -evaluate();
    Move mv[256]; int n = gen_legal(white, mv);
    if (n == 0) return in_check(white) ? -(MATE + depth) : 0;   /* mate (prefer slower) / stalemate */
    char bak[65];
    int best = -INF;
    for (int i = 0; i < n; i++) {
        apply(mv[i], bak);
        int sc = -negamax(depth - 1, -beta, -alpha, !white);
        undo(bak);
        if (sc > best) best = sc;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    return best;
}

/* Pick and play Black's move. Returns 0 if Black has no move (handled by caller). */
static int cpu_move(void) {
    Move mv[256]; int n = gen_legal(0, mv);
    if (n == 0) return 0;
    char bak[65];
    int best = -INF, bi = 0;
    for (int i = 0; i < n; i++) {
        apply(mv[i], bak);
        int sc = -negamax(2, -INF, INF, 1);   /* total depth 3 from Black's root */
        undo(bak);
        if (sc > best) { best = sc; bi = i; }
    }
    apply(mv[bi], bak);
    lfrom = mv[bi].from; lto = mv[bi].to;
    return 1;
}

static void putn(int n) {
    char t[8]; int i = 0; if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}

static void render(void) {
    sys_clear();
    sys_setcolor(4); print("  Chess"); sys_setcolor(0);
    print("  you=White  CPU=Black\n");
    for (int r = 0; r < 8; r++) {
        print("  "); sys_setcolor(8); putn(8 - r); print(" "); sys_setcolor(0);
        for (int c = 0; c < 8; c++) {
            int idx = r*8 + c;
            char p = bd[idx];
            int iscur = (r == cr && c == cc), issel = (sel == idx);
            int islast = (idx == lfrom || idx == lto);
            if (issel)        sys_setcolor(10);               /* picked piece: green    */
            else if (iscur)   sys_setcolor(14);               /* cursor: yellow         */
            else if (islast)  sys_setcolor(11);               /* last move: bright cyan */
            else if (empty(p)) sys_setcolor(8);               /* empty: dim             */
            else sys_setcolor(is_w(p) ? 15 : 12);             /* White bright / Black red */
            char two[3]; two[0] = empty(p) ? (idx == lfrom ? '*' : '.') : p; two[1] = ' '; two[2] = 0;
            print(two);
        }
        sys_setcolor(0); print("\n");
    }
    sys_setcolor(8); print("    a b c d e f g h\n"); sys_setcolor(0);
    if (over) sys_setcolor(2); else sys_setcolor(0);
    print("  "); print(msg); print("\n");
    sys_setcolor(0);
    print("  arrows move  space pick/move  r q\n");
}

/* After a side moves, set the status line / detect end of game from the other
 * side's reply options. `mover_white` just moved; check the side to move next. */
static void check_state(int next_white) {
    Move mv[256]; int n = gen_legal(next_white, mv);
    int chk = in_check(next_white);
    if (n == 0) {
        over = 1;
        if (chk) msg = next_white ? "Checkmate - you lose." : "Checkmate - you win!";
        else     msg = "Stalemate - draw.";
    } else {
        msg = chk ? (next_white ? "Check! your move." : "Check to Black.")
                  : (next_white ? "Your move (White)." : "Black to move.");
    }
}

int main(void) {
    reset();
    render();
    for (;;) {
        int k = sys_pollkey();
        if (k < 0) { sys_sleep(20); continue; }
        if (k == 'q' || k == 'Q') return 0;
        if (k == 'r' || k == 'R') { reset(); render(); continue; }
        if (over) continue;
        if      (k == 0x11 && cr > 0) cr--;
        else if (k == 0x12 && cr < 7) cr++;
        else if (k == 0x13 && cc > 0) cc--;
        else if (k == 0x14 && cc < 7) cc++;
        else if (k == ' ') {
            int idx = cr*8 + cc;
            if (sel < 0) {
                if (is_w(bd[idx])) sel = idx;             /* pick up one of our pieces */
            } else if (idx == sel) {
                sel = -1;                                 /* tap again to cancel */
            } else if (is_w(bd[idx])) {
                sel = idx;                                /* re-pick another piece */
            } else {
                /* attempt the move sel -> idx if it's legal */
                Move mv[256]; int n = gen_legal(1, mv), done = 0;
                for (int i = 0; i < n; i++) if (mv[i].from == sel && mv[i].to == idx) {
                    char bak[65]; apply(mv[i], bak);
                    lfrom = mv[i].from; lto = mv[i].to;
                    sel = -1; done = 1;
                    sys_beep(660, 40);
                    check_state(0);                       /* is Black (to move) mated? */
                    render();
                    if (!over) {
                        sys_sleep(120);
                        cpu_move();                       /* Black replies */
                        sys_beep(440, 40);
                        check_state(1);                   /* is White (to move) mated? */
                    }
                    break;
                }
                if (!done) sys_beep(160, 80);             /* illegal: reject */
            }
        }
        render();
    }
}
