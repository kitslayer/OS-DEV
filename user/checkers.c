/*
 * checkers.c — Checkers (draughts) against the computer.
 *
 * Standard American rules on the dark squares of an 8x8 board: men move and
 * capture one diagonal forward, kings both ways; capturing is MANDATORY and a
 * multi-jump must be continued; a man reaching the far row is crowned. You are
 * red (o / O for a king) at the bottom and move up; the CPU is white (x / X)
 * and moves down. Take all the CPU's pieces (or leave it with no move) to win.
 *
 * Arrows move the cursor, Space selects one of your pieces then a destination
 * (legal targets are marked '*'), r restarts, q quits.
 */
#include "ulib.h"

static int b[8][8];                 /* 0 empty; you: 1 man 2 king (move up); cpu: -1 man -2 king (move down) */
static int cr = 5, cc = 0, sr = -1, sc = -1;   /* cursor + selected square */
static int turn = 1;                /* 1 = you, -1 = cpu */
static int over, youwin;
static const char *msg;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void putn(int n) { if (n >= 10) { char t[2] = { (char)('0' + n/10), 0 }; print(t); } char u[2] = { (char)('0' + n%10), 0 }; print(u); }

static int own(int v, int s)   { return v != 0 && ((v > 0) == (s > 0)); }
static int enemy(int v, int s) { return v != 0 && ((v > 0) != (s > 0)); }
static int king(int v)         { return v == 2 || v == -2; }
static int in(int r, int c)    { return r >= 0 && r < 8 && c >= 0 && c < 8; }

/* the diagonal step directions a piece at (r,c) may use */
static int dirs(int r, int c, int dr[4], int dc[4]) {
    int v = b[r][c], n = 0;
    int up = (v > 0);                                  /* you move up (dr=-1) */
    if (king(v) || up)   { dr[n]=-1; dc[n]=-1; n++; dr[n]=-1; dc[n]=1; n++; }
    if (king(v) || !up)  { dr[n]= 1; dc[n]=-1; n++; dr[n]= 1; dc[n]=1; n++; }
    return n;
}

/* can the piece at (r,c) capture? */
static int piece_can_jump(int r, int c) {
    if (b[r][c] == 0) return 0;
    int s = b[r][c] > 0 ? 1 : -1, dr[4], dc[4], n = dirs(r, c, dr, dc);
    for (int i = 0; i < n; i++) {
        int mr = r+dr[i], mc = c+dc[i], lr = r+2*dr[i], lc = c+2*dc[i];
        if (in(lr,lc) && enemy(b[mr][mc], s) && b[lr][lc] == 0) return 1;
    }
    return 0;
}
static int side_has_jump(int s) {
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) if (own(b[r][c], s) && piece_can_jump(r,c)) return 1;
    return 0;
}
static int side_has_move(int s) {
    if (side_has_jump(s)) return 1;
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) if (own(b[r][c], s)) {
        int dr[4], dc[4], n = dirs(r,c,dr,dc);
        for (int i = 0; i < n; i++) if (in(r+dr[i],c+dc[i]) && b[r+dr[i]][c+dc[i]] == 0) return 1;
    }
    return 0;
}

/* is (tr,tc) a legal destination for the selected piece (sr,sc)? sets *isjump.
 * Honors the mandatory-capture rule (mustjump). */
static int legal_dest(int fr, int fc, int tr, int tc, int mustjump, int *isjump) {
    if (!in(tr,tc) || b[tr][tc] != 0) return 0;
    int s = b[fr][fc] > 0 ? 1 : -1, dr[4], dc[4], n = dirs(fr,fc,dr,dc);
    for (int i = 0; i < n; i++) {
        if (tr == fr+dr[i] && tc == fc+dc[i]) { *isjump = 0; return !mustjump; }   /* simple step */
        if (tr == fr+2*dr[i] && tc == fc+2*dc[i]) {                                 /* jump */
            int mr = fr+dr[i], mc = fc+dc[i];
            if (enemy(b[mr][mc], s) && b[tr][tc] == 0) { *isjump = 1; return 1; }
        }
    }
    return 0;
}

static void crown(int r, int c) {
    if (b[r][c] == 1 && r == 0) b[r][c] = 2;          /* your man reaches the top */
    if (b[r][c] == -1 && r == 7) b[r][c] = -2;        /* cpu man reaches the bottom */
}

/* apply a move; returns 1 if it was a capture */
static int do_move(int fr, int fc, int tr, int tc) {
    int jump = (tr - fr == 2 || fr - tr == 2);
    b[tr][tc] = b[fr][fc]; b[fr][fc] = 0;
    if (jump) b[(fr+tr)/2][(fc+tc)/2] = 0;            /* remove the captured piece */
    crown(tr, tc);
    return jump;
}

static int count(int s) { int n=0; for(int r=0;r<8;r++)for(int c=0;c<8;c++) if(own(b[r][c],s)) n++; return n; }

static void check_end(void) {
    if (count(-1) == 0 || !side_has_move(-1)) { over = 1; youwin = 1; msg = "You WIN!  (r replay)"; sys_beep(880,180); }
    else if (count(1) == 0 || !side_has_move(1)) { over = 1; youwin = 0; msg = "CPU wins.  (r replay)"; sys_beep(165,260); }
}

/* CPU: greedy — must capture if able (and continue multi-jumps); else a random legal slide. */
static void cpu_turn(void) {
    for (;;) {
        if (side_has_jump(-1)) {
            /* find any cpu piece that can jump, prefer continuing a chain */
            int fr=-1,fc=-1,tr=-1,tc=-1;
            for (int r=0;r<8&&fr<0;r++) for (int c=0;c<8&&fr<0;c++) if (own(b[r][c],-1) && piece_can_jump(r,c)) {
                int dr[4],dc[4],n=dirs(r,c,dr,dc);
                for (int i=0;i<n;i++){ int mr=r+dr[i],mc=c+dc[i],lr=r+2*dr[i],lc=c+2*dc[i];
                    if (in(lr,lc)&&enemy(b[mr][mc],-1)&&b[lr][lc]==0){ fr=r;fc=c;tr=lr;tc=lc;break; } }
            }
            if (fr<0) break;
            do_move(fr,fc,tr,tc);
            if (piece_can_jump(tr,tc)) {                 /* multi-jump continuation */
                /* re-select the same piece next loop by leaving it; simplest: loop again, it'll be found */
                continue;
            }
            break;
        } else {
            /* collect all legal slides, pick one at random (favouring forward already by men dirs) */
            int fr[64],fc[64],tr[64],tc[64],m=0;
            for (int r=0;r<8;r++) for (int c=0;c<8;c++) if (own(b[r][c],-1)) {
                int dr[4],dc[4],n=dirs(r,c,dr,dc);
                for (int i=0;i<n;i++){ int nr=r+dr[i],nc=c+dc[i];
                    if (in(nr,nc)&&b[nr][nc]==0&&m<64){ fr[m]=r;fc[m]=c;tr[m]=nr;tc[m]=nc;m++; } }
            }
            if (m==0) break;
            int p = (int)(rnd()%(unsigned)m);
            do_move(fr[p],fc[p],tr[p],tc[p]);
            break;
        }
    }
    turn = 1;
    if (!over) { check_end(); if (!over) msg = side_has_jump(1) ? "Your move - you MUST jump" : "Your move"; }
}

static void render(void) {
    sys_clear();
    int mj = (turn==1) && side_has_jump(1);
    sys_setcolor(4); print("\n  Checkers"); sys_setcolor(0);
    print("   you "); sys_setcolor(2); putn(count(1));
    print("  cpu "); sys_setcolor(7); putn(count(-1)); sys_setcolor(0); print("\n\n");
    for (int r = 0; r < 8; r++) {
        print("    ");
        for (int c = 0; c < 8; c++) {
            int v = b[r][c], isj, cur=(r==cr&&c==cc), selpc=(r==sr&&c==sc);
            int dest = (sr>=0) && legal_dest(sr,sc,r,c,mj,&isj);
            char ch = v==1?'o':v==2?'O':v==-1?'x':v==-2?'X':(dest?'*':((r+c)&1?'.':' '));
            int col = v>0?2:v<0?7:(dest?10:8);
            if (selpc) col = 11; else if (cur) col = 15;
            sys_setcolor(col);
            char cell[4]; cell[0]=cur?'[':selpc?'(':' '; cell[1]=ch; cell[2]=cur?']':selpc?')':' '; cell[3]=0;
            print(cell);
        }
        sys_setcolor(0); print("\n");
    }
    print("\n  "); print(msg);
    print("\n  arrows move  space select/move  r reset  q quit\n");
}

static void reset(void) {
    for (int r=0;r<8;r++) for (int c=0;c<8;c++) {
        if ((r+c)&1) { if (r<3) b[r][c]=-1; else if (r>4) b[r][c]=1; else b[r][c]=0; }
        else b[r][c]=0;
    }
    cr=5; cc=0; sr=sc=-1; turn=1; over=0; youwin=0; msg="Your move";
}

int main(void) {
    rng=(unsigned)sys_uptime_ms()|1u;
    reset(); render();
    for (;;) {
        int k = sys_pollkey();
        if (k<0){ sys_sleep(20); continue; }
        if (k=='q'||k=='Q') break;
        if (k=='r'||k=='R'){ reset(); render(); continue; }
        if (over || turn!=1) continue;
        if      (k==0x11){ if(cr>0)cr--; render(); }
        else if (k==0x12){ if(cr<7)cr++; render(); }
        else if (k==0x13){ if(cc>0)cc--; render(); }
        else if (k==0x14){ if(cc<7)cc++; render(); }
        else if (k==' '||k=='\n'||k=='\r') {
            int mj = side_has_jump(1);
            if (sr<0) {                                   /* select a piece */
                if (own(b[cr][cc],1) && (!mj || piece_can_jump(cr,cc))) { sr=cr; sc=cc; }
            } else if (cr==sr && cc==sc) { sr=sc=-1; }     /* deselect */
            else {
                int isj;
                if (legal_dest(sr,sc,cr,cc,mj,&isj)) {
                    int jr=cr, jc=cc; do_move(sr,sc,cr,cc);
                    if (isj && piece_can_jump(jr,jc)) { sr=jr; sc=jc; msg="Keep jumping!"; }  /* must continue */
                    else { sr=sc=-1; check_end(); if(!over) cpu_turn(); }
                }
            }
            render();
        }
    }
    return 0;
}
