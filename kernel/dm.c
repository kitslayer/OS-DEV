/*
 * dm.c — device-mapper-lite: a virtual block device composed from registered
 * blockdev_t children (M1157). A RAID-1 MIRROR writes every sector to both
 * members and reads from either, so the volume survives a member failure. It
 * works on ANY storage driver because it fans its read/write out through the
 * uniform blockdev_read/blockdev_write (kernel/blockdev.c) — the block layer is
 * already a vtable, so a mirror is just another consumer of it.
 *
 * dm_selftest() runs at boot IFF two non-boot writable disks are present (e.g.
 * osdrive --disk2 --disk3); with fewer it is a clean no-op, so the normal boot
 * + every make-check suite is unaffected.
 */
#include "dm.h"
#include "blockdev.h"
#include "console.h"
#include <stdint.h>

#define DM_SECSZ 512

/* RAID-1 read: serve from member A, falling back to B (e.g. if A has been
 * marked failed or its read errors) — the redundancy guarantee. */
int dm_mirror_read(dm_mirror_t *m, uint64_t lba, uint32_t count, void *buf) {
    if (!m->a_failed && blockdev_read(m->a, lba, count, buf) == 0) return 0;
    if (!m->b_failed && blockdev_read(m->b, lba, count, buf) == 0) return 0;
    return -1;
}

/* RAID-1 write: fan out to BOTH members (skipping a failed one). Succeeds only
 * if every live member took the write. */
int dm_mirror_write(dm_mirror_t *m, uint64_t lba, uint32_t count, const void *buf) {
    int ra = m->a_failed ? 0 : blockdev_write(m->a, lba, count, buf);
    int rb = m->b_failed ? 0 : blockdev_write(m->b, lba, count, buf);
    return (ra == 0 && rb == 0) ? 0 : -1;
}

/* ---- RAID-0 striping (M1291): round-robin 1-sector chunks across members --- */
int dm_stripe_read(dm_stripe_t *s, uint64_t lba, uint32_t count, void *buf) {
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++, lba++, p += DM_SECSZ)
        if (blockdev_read(s->devs[lba % s->n], lba / s->n, 1, p) != 0) return -1;
    return 0;
}
int dm_stripe_write(dm_stripe_t *s, uint64_t lba, uint32_t count, const void *buf) {
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++, lba++, p += DM_SECSZ)
        if (blockdev_write(s->devs[lba % s->n], lba / s->n, 1, p) != 0) return -1;
    return 0;
}

/* ---- RAID-5 rotating parity (M1291) --------------------------------------- *
 * Stripe `s` stores its parity on member (s % n); the other members hold its
 * (n-1) data sectors in order. Every member keeps stripe s at physical sector s.
 * Parity = XOR of the stripe's data sectors, so any one lost sector is the XOR
 * of all the surviving members' sectors in that stripe. */
static int raid5_member(const dm_raid5_t *r, uint64_t stripe, int pos) {
    int pm = (int)(stripe % r->n), seen = 0;
    for (int m = 0; m < r->n; m++) {
        if (m == pm) continue;
        if (seen == pos) return m;
        seen++;
    }
    return -1;
}

static int dm_raid5_read1(dm_raid5_t *r, uint64_t lba, void *buf) {
    int nd = r->n - 1;
    uint64_t stripe = lba / nd;
    int dmem = raid5_member(r, stripe, (int)(lba % nd));
    if (dmem < 0) return -1;
    if (!r->failed[dmem]) return blockdev_read(r->devs[dmem], stripe, 1, buf);
    /* degraded: reconstruct dmem's sector as the XOR of EVERY other member's
     * sector `stripe` (the surviving data sectors XOR the parity sector). */
    uint8_t *out = buf, tmp[DM_SECSZ];
    for (int b = 0; b < DM_SECSZ; b++) out[b] = 0;
    for (int m = 0; m < r->n; m++) {
        if (m == dmem) continue;
        if (r->failed[m]) return -1;                       /* a second failure -> unrecoverable */
        if (blockdev_read(r->devs[m], stripe, 1, tmp) != 0) return -1;
        for (int b = 0; b < DM_SECSZ; b++) out[b] ^= tmp[b];
    }
    return 0;
}

static int dm_raid5_write1(dm_raid5_t *r, uint64_t lba, const void *buf) {
    int nd = r->n - 1;
    uint64_t stripe = lba / nd;
    int pm = (int)(stripe % r->n);
    int dmem = raid5_member(r, stripe, (int)(lba % nd));
    if (dmem < 0 || r->failed[dmem]) return -1;            /* writing a failed data member: unsupported */
    if (r->failed[pm])                                     /* parity down: write data only (rebuilt on resync) */
        return blockdev_write(r->devs[dmem], stripe, 1, buf);
    /* read-modify-write parity: new_par = old_par XOR old_data XOR new_data. */
    uint8_t olddata[DM_SECSZ], par[DM_SECSZ];
    if (blockdev_read(r->devs[dmem], stripe, 1, olddata) != 0) return -1;
    if (blockdev_read(r->devs[pm], stripe, 1, par) != 0) return -1;
    const uint8_t *nb = buf;
    for (int b = 0; b < DM_SECSZ; b++) par[b] ^= olddata[b] ^ nb[b];
    if (blockdev_write(r->devs[dmem], stripe, 1, buf) != 0) return -1;
    return blockdev_write(r->devs[pm], stripe, 1, par);
}

int dm_raid5_read(dm_raid5_t *r, uint64_t lba, uint32_t count, void *buf) {
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++, lba++, p += DM_SECSZ)
        if (dm_raid5_read1(r, lba, p) != 0) return -1;
    return 0;
}
int dm_raid5_write(dm_raid5_t *r, uint64_t lba, uint32_t count, const void *buf) {
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++, lba++, p += DM_SECSZ)
        if (dm_raid5_write1(r, lba, p) != 0) return -1;
    return 0;
}

/* ---- linear logical volume (LVM-lite, M1296): concatenate members --------- */
/* Map a volume-logical sector to (member blockdev index, member-local sector)
 * by walking the per-member sizes; returns the blockdev index, or -1 past end. */
static int dm_linear_locate(const dm_linear_t *l, uint64_t lba, uint64_t *local) {
    for (int m = 0; m < l->n; m++) {
        if (lba < l->sectors[m]) { *local = lba; return l->devs[m]; }
        lba -= l->sectors[m];
    }
    return -1;
}
int dm_linear_read(dm_linear_t *l, uint64_t lba, uint32_t count, void *buf) {
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++, lba++, p += DM_SECSZ) {
        uint64_t loc; int dev = dm_linear_locate(l, lba, &loc);
        if (dev < 0 || blockdev_read(dev, loc, 1, p) != 0) return -1;
    }
    return 0;
}
int dm_linear_write(dm_linear_t *l, uint64_t lba, uint32_t count, const void *buf) {
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++, lba++, p += DM_SECSZ) {
        uint64_t loc; int dev = dm_linear_locate(l, lba, &loc);
        if (dev < 0 || blockdev_write(dev, loc, 1, p) != 0) return -1;
    }
    return 0;
}

/* Boot self-test: build RAID-1 / RAID-0 / RAID-5 volumes over the non-boot
 * writable disks and prove each one's defining property — mirror redundancy,
 * stripe distribution, and RAID-5 single-disk fault tolerance via parity
 * reconstruction. RAID-5 needs >=3 members; with exactly 2 only RAID-1/0 run;
 * with <2 it's a clean no-op (so the normal boot + every make-check suite is
 * unaffected). Touches one scratch sector per member (physical LBA 64, clear of
 * any superblock) and restores them all at the end. Logs to dmesg. */
void dm_selftest(void) {
    int devs[DM_MAXDEV], nd = 0, n = blockdev_count();
    for (int i = 0; i < n && nd < DM_MAXDEV; i++) {
        blockdev_t *d = blockdev_get(i);
        if (!d || !d->write || !d->name) continue;
        if (d->name[0]=='a' && d->name[1]=='t' && d->name[2]=='a') continue;   /* never the boot disk */
        devs[nd++] = i;
    }
    if (nd < 2) { kprintf("[dm] RAID self-test skipped (need >= 2 non-boot writable disks)\n"); return; }

    const uint64_t lba = 64;                        /* scratch sector, clear of any superblock */
    static uint8_t save[DM_MAXDEV][DM_SECSZ], pat[DM_SECSZ], back[DM_SECSZ];
    for (int k = 0; k < nd; k++)
        if (blockdev_read(devs[k], lba, 1, save[k]) != 0) { kprintf("[dm] RAID self-test: scratch save failed\n"); return; }

    /* ---- RAID-1 mirror: fan-out write + read survives a member failure ---- */
    {
        dm_mirror_t m = { devs[0], devs[1], 0, 0 };
        for (int b = 0; b < DM_SECSZ; b++) pat[b] = (uint8_t)(b * 13 + 0x37);
        int rt = dm_mirror_write(&m, lba, 1, pat) == 0 && dm_mirror_read(&m, lba, 1, back) == 0;
        for (int b = 0; rt && b < DM_SECSZ; b++) if (back[b] != pat[b]) rt = 0;
        int fan = blockdev_read(devs[1], lba, 1, back) == 0;              /* B holds it directly */
        for (int b = 0; fan && b < DM_SECSZ; b++) if (back[b] != pat[b]) fan = 0;
        m.a_failed = 1;
        int red = dm_mirror_read(&m, lba, 1, back) == 0;                  /* must serve from B */
        for (int b = 0; red && b < DM_SECSZ; b++) if (back[b] != pat[b]) red = 0;
        if (rt && fan && red) kprintf("[ ok ] dm RAID-1 mirror OK (fan-out + read survives member failure)\n");
        else kprintf("[dm] RAID-1 FAILED (rt=%d fan=%d red=%d)\n", rt, fan, red);
    }

    /* ---- RAID-0 stripe: consecutive logical sectors land on distinct members ----
     * logical L = 64*nd .. 64*nd+nd-1 map to physical sector 64 on members 0..nd-1
     * (L/nd == 64, L%nd == member); a distinct pattern per member proves the split. */
    {
        dm_stripe_t s; s.n = nd; for (int k = 0; k < nd; k++) s.devs[k] = devs[k];
        uint64_t base = (uint64_t)64 * nd;
        int ok = 1;
        for (int k = 0; k < nd && ok; k++) {
            for (int b = 0; b < DM_SECSZ; b++) pat[b] = (uint8_t)(b + k * 7 + 1);
            if (dm_stripe_write(&s, base + k, 1, pat) != 0) ok = 0;
        }
        for (int k = 0; k < nd && ok; k++) {                              /* each member holds its own chunk */
            if (blockdev_read(devs[k], 64, 1, back) != 0) { ok = 0; break; }
            for (int b = 0; b < DM_SECSZ; b++) if (back[b] != (uint8_t)(b + k * 7 + 1)) { ok = 0; break; }
        }
        for (int k = 0; k < nd && ok; k++) {                              /* and reads come back through the stripe */
            if (dm_stripe_read(&s, base + k, 1, back) != 0) { ok = 0; break; }
            for (int b = 0; b < DM_SECSZ; b++) if (back[b] != (uint8_t)(b + k * 7 + 1)) { ok = 0; break; }
        }
        if (ok) kprintf("[ ok ] dm RAID-0 stripe OK (%d members, round-robin distribution)\n", nd);
        else kprintf("[dm] RAID-0 FAILED\n");
    }

    /* ---- RAID-5 parity: survive ANY single-member loss via XOR reconstruct ---- */
    if (nd >= 3) {
        dm_raid5_t r; r.n = nd; for (int k = 0; k < nd; k++) { r.devs[k] = devs[k]; r.failed[k] = 0; }
        int ndata = nd - 1;
        uint64_t stripe = 64, base = stripe * (uint64_t)ndata;
        int pm = (int)(stripe % nd);
        int ok = 1;
        static uint8_t zero[DM_SECSZ];
        for (int b = 0; b < DM_SECSZ; b++) zero[b] = 0;
        for (int k = 0; k < nd && ok; k++) if (blockdev_write(devs[k], 64, 1, zero) != 0) ok = 0;  /* parity 0 == XOR of zeros */
        for (int pos = 0; pos < ndata && ok; pos++) {                     /* RMW-write keeps parity consistent */
            for (int b = 0; b < DM_SECSZ; b++) pat[b] = (uint8_t)(b * 3 + pos * 29 + 5);
            if (dm_raid5_write(&r, base + pos, 1, pat) != 0) ok = 0;
        }
        int normal = ok;
        for (int pos = 0; pos < ndata && normal; pos++) {                 /* normal read-back matches */
            if (dm_raid5_read(&r, base + pos, 1, back) != 0) { normal = 0; break; }
            for (int b = 0; b < DM_SECSZ; b++) if (back[b] != (uint8_t)(b * 3 + pos * 29 + 5)) { normal = 0; break; }
        }
        int parok = normal;                                               /* parity sector == XOR of all data sectors */
        if (parok) {
            uint8_t acc[DM_SECSZ]; for (int b = 0; b < DM_SECSZ; b++) acc[b] = 0;
            for (int pos = 0; pos < ndata; pos++) for (int b = 0; b < DM_SECSZ; b++) acc[b] ^= (uint8_t)(b * 3 + pos * 29 + 5);
            if (blockdev_read(devs[pm], 64, 1, back) != 0) parok = 0;
            for (int b = 0; parok && b < DM_SECSZ; b++) if (back[b] != acc[b]) parok = 0;
        }
        int dead = raid5_member(&r, stripe, 0);                           /* fail the member holding data pos 0 */
        r.failed[dead] = 1;
        int deg = dm_raid5_read(&r, base + 0, 1, back) == 0;              /* reconstruct it from parity + survivors */
        for (int b = 0; deg && b < DM_SECSZ; b++) if (back[b] != (uint8_t)(b * 3 + 0 * 29 + 5)) deg = 0;
        r.failed[dead] = 0;
        if (normal && parok && deg) kprintf("[ ok ] dm RAID-5 parity OK (%d members, single-disk fault reconstructed)\n", nd);
        else kprintf("[dm] RAID-5 FAILED (normal=%d parity=%d degraded=%d)\n", normal, parok, deg);
    }

    /* ---- linear LV: concatenate the members into one address space ----
     * With per-member size 128, logical 64 -> member0 phys 64 and logical 192
     * -> member1 phys 64 (192-128=64) — both the scratch sector saved above —
     * so a single logical address space spans the disk boundary at logical 128. */
    {
        dm_linear_t lv; lv.n = nd; for (int k = 0; k < nd; k++) { lv.devs[k] = devs[k]; lv.sectors[k] = 128; }
        static uint8_t v[DM_SECSZ];
        int ok = 1;
        for (int b = 0; b < DM_SECSZ; b++) pat[b]  = (uint8_t)(b * 5 + 0x11);   /* pattern -> member 0 */
        for (int b = 0; b < DM_SECSZ; b++) back[b] = (uint8_t)(b * 5 + 0x22);   /* pattern -> member 1 */
        if (dm_linear_write(&lv, 64,  1, pat)  != 0) ok = 0;                    /* logical 64  -> member0 phys 64 */
        if (ok && dm_linear_write(&lv, 192, 1, back) != 0) ok = 0;             /* logical 192 -> member1 phys 64 (crossed the boundary) */
        if (ok && blockdev_read(devs[0], 64, 1, v) == 0)                        /* member 0 directly holds pattern 1 */
            for (int b = 0; b < DM_SECSZ; b++) if (v[b] != (uint8_t)(b*5+0x11)) { ok = 0; break; }
        if (ok && blockdev_read(devs[1], 64, 1, v) == 0)                        /* member 1 directly holds pattern 2 */
            for (int b = 0; b < DM_SECSZ; b++) if (v[b] != (uint8_t)(b*5+0x22)) { ok = 0; break; }
        if (ok && dm_linear_read(&lv, 192, 1, v) == 0)                          /* and a read THROUGH the LV returns it */
            for (int b = 0; b < DM_SECSZ; b++) if (v[b] != (uint8_t)(b*5+0x22)) { ok = 0; break; }
        if (ok) kprintf("[ ok ] dm linear LV OK (%d members concatenated; logical space spans the disk boundary)\n", nd);
        else kprintf("[dm] linear LV FAILED\n");
    }

    for (int k = 0; k < nd; k++) blockdev_write(devs[k], lba, 1, save[k]);  /* restore every scratch sector */
    kprintf("[dm] RAID self-test complete (%d members)\n", nd);
}
