/*
 * yahtzee.c — solo Yahtzee.
 *
 * Each of 13 turns: the five dice are rolled, then you may re-roll any of them
 * up to two more times (toggle holds with 1-5, press r to re-roll), and finally
 * assign the dice to one of the 13 scoring categories (arrows to pick an empty
 * row, Enter to score it). Fill all 13; reach 63+ in the upper section for a
 * +35 bonus. Best total is saved to YAHTZEE.HI.
 */
#include "ulib.h"

#define NCAT 13
static const char *NAME[NCAT] = {
    "Ones", "Twos", "Threes", "Fours", "Fives", "Sixes",
    "3 of a kind", "4 of a kind", "Full house", "Sm straight", "Lg straight",
    "YAHTZEE", "Chance"
};
static int dice[5], hold[5], rolls, dval[NCAT], done[NCAT], turn, cur, over, hi;

static unsigned rng;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void putn(int n) {
    char t[8]; int i = 0; if (n == 0) { print("0"); return; }
    while (n) { t[i++] = (char)('0' + n % 10); n /= 10; }
    char s[8]; int j = 0; while (i) s[j++] = t[--i]; s[j] = 0; print(s);
}
static void load_hi(void){ char b[16]; long n=sys_readfile("YAHTZEE.HI",b,15); hi=0; for(long i=0;i<n;i++){ if(b[i]<'0'||b[i]>'9')break; hi=hi*10+(b[i]-'0'); } }
static void save_hi(void){ char t[12],b[12]; int i=0,n=0,v=hi; if(v==0)t[i++]='0'; while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)b[n++]=t[--i]; sys_writefile("YAHTZEE.HI",b,(unsigned long)n); }

/* potential score of the current dice in category c */
static int score_of(int c) {
    int cnt[7] = {0}, sum = 0;
    for (int i = 0; i < 5; i++) { cnt[dice[i]]++; sum += dice[i]; }
    if (c <= 5) return cnt[c + 1] * (c + 1);
    switch (c) {
    case 6: for (int f = 1; f <= 6; f++) if (cnt[f] >= 3) return sum; return 0;
    case 7: for (int f = 1; f <= 6; f++) if (cnt[f] >= 4) return sum; return 0;
    case 8: { int t3 = 0, t2 = 0; for (int f = 1; f <= 6; f++) { if (cnt[f] == 3) t3 = 1; if (cnt[f] == 2) t2 = 1; } return (t3 && t2) ? 25 : 0; }
    case 9: { for (int s = 1; s <= 3; s++) if (cnt[s] && cnt[s+1] && cnt[s+2] && cnt[s+3]) return 30; return 0; }
    case 10: { for (int s = 1; s <= 2; s++) if (cnt[s] && cnt[s+1] && cnt[s+2] && cnt[s+3] && cnt[s+4]) return 40; return 0; }
    case 11: for (int f = 1; f <= 6; f++) if (cnt[f] == 5) return 50; return 0;
    case 12: return sum;
    }
    return 0;
}
static int upper_total(void){ int t=0; for(int c=0;c<=5;c++) if(done[c]) t+=dval[c]; return t; }
static int grand_total(void){ int t=0; for(int c=0;c<NCAT;c++) if(done[c]) t+=dval[c]; if(upper_total()>=63) t+=35; return t; }

static void roll(void){ for(int i=0;i<5;i++) if(!hold[i]) dice[i]=1+(int)(rnd()%6); rolls--; }

static void new_turn(void){
    for(int i=0;i<5;i++) hold[i]=0;
    rolls=3; roll();                    /* first (mandatory) roll */
    /* park the cursor on the first empty category */
    cur=0; while(cur<NCAT && done[cur]) cur++;
}
static void reset(void){
    for(int c=0;c<NCAT;c++){ done[c]=0; dval[c]=0; }
    turn=0; over=0; new_turn();
}

static void render(void){
    sys_clear();
    sys_setcolor(4); print("  Yahtzee"); sys_setcolor(0);
    print("  total "); sys_setcolor(2); putn(grand_total()); sys_setcolor(0);
    print("  hi "); sys_setcolor(14); putn(hi); sys_setcolor(0); print("\n  dice:");
    for(int i=0;i<5;i++){ sys_setcolor(hold[i]?14:7); print(" "); putn(dice[i]); print(hold[i]?"*":" "); }
    sys_setcolor(8); print("  rolls "); putn(rolls<0?0:rolls); sys_setcolor(0); print("\n");
    for(int c=0;c<NCAT;c++){
        int sel = (c==cur && !over);
        sys_setcolor(sel?11:(done[c]?8:0)); print(sel?" > ":"   ");
        print(NAME[c]);
        int pad = 12 - (int)ustrlen(NAME[c]); while(pad-->0) print(" ");
        if(done[c]){ sys_setcolor(8); putn(dval[c]); }
        else { sys_setcolor(10); print("("); putn(score_of(c)); print(")"); }   /* potential */
        sys_setcolor(0); print("\n");
    }
    print("  ");
    if(over){ sys_setcolor(grand_total()>=hi?10:0); print("Game over - total "); putn(grand_total()); print("   r new game"); sys_setcolor(0); }
    else print("1-5 hold  r roll  Enter score  q quit");
    print("\n");
}

int main(void){
    rng=(unsigned)sys_uptime_ms()|1u;
    load_hi(); reset(); render();
    for(;;){
        int k=sys_pollkey();
        if(k<0){ sys_sleep(20); continue; }
        if(k=='q'||k=='Q') break;
        if(k=='r'||k=='R'){ if(over){ reset(); } else if(rolls>0){ roll(); } render(); continue; }
        if(over) continue;
        if(k>='1'&&k<='5'){ int i=k-'1'; hold[i]^=1; render(); }
        else if(k==0x11){ do{ cur=(cur+NCAT-1)%NCAT; }while(done[cur]); render(); }
        else if(k==0x12){ do{ cur=(cur+1)%NCAT; }while(done[cur]); render(); }
        else if(k=='\n'||k=='\r'){
            if(done[cur]) continue;
            dval[cur]=score_of(cur); done[cur]=1; turn++;
            sys_beep(660,40);
            if(turn>=NCAT){ over=1; int g=grand_total(); if(g>hi){ hi=g; save_hi(); } sys_beep(1046,200); }
            else new_turn();
            render();
        }
    }
    return 0;
}
