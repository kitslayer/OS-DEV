/* smp.c — multiprocessor bring-up: enable the local APIC, enumerate the cores
 * from the ACPI MADT, and start each application processor (AP).
 *
 * Flow (all on the bootstrap processor / BSP):
 *   1. lapic_enable_this_cpu()   — software-enable the BSP's local APIC.
 *   2. acpi_madt_lapics()        — read the APIC IDs of all CPUs from the MADT.
 *   3. copy the trampoline blob to physical 0x8000 + fill the param block.
 *   4. for each AP: INIT IPI, then two STARTUP IPIs (vector = 0x8000>>12 = 8).
 *      Each AP climbs ap_trampoline.asm into long mode and lands in ap_main(),
 *      which adopts the kernel GDT+IDT, bumps smp_cpu_count, and idles in hlt.
 *
 * The APs do NOT run the general scheduler. They idle halted and only wake on a
 * fixed inter-processor interrupt (vector 0x40) to drain the parallel job pool
 * (smp_parallel_for) — pure-compute work, the one concurrent path here — then
 * halt again. So aside from that small pool, no other subsystem has to be
 * SMP-safe yet. The legacy 8259 PIC is left exactly as pic_init() left it
 * (remapped + fully masked): the LAPIC and PIC coexist, and all IPIs travel over
 * the LAPIC's ICR, independent of the PIC. Verified on QEMU with -smp N (see the
 * [smp] lines in the boot log).
 */
#include "smp.h"
#include "smpthread.h"  /* smpthread_ap_tick — real kernel threads pinned per-core (M1530) */
#include "acpi.h"
#include "vmm.h"
#include "io.h"
#include "console.h"
#include "gdt.h"
#include "idt.h"
#include <stdint.h>

#define LAPIC_PHYS  0xFEE00000ull       /* xAPIC MMIO register block (1 page) */
#define TRAMP_PHYS  0x8000ull           /* SIPI vector 0x08 -> physical 0x8000 */
#define AP_PARAM    0x7000ull           /* parameter block the trampoline reads */
#define MAX_CPUS    16

/* LAPIC register offsets (bytes from LAPIC_PHYS; all 32-bit, dword-aligned). */
#define LAPIC_ID    0x020               /* this CPU's APIC ID in bits 24..31 */
#define LAPIC_TPR   0x080               /* task priority (0 = accept everything) */
#define LAPIC_SVR   0x0F0               /* spurious-interrupt vector + enable bit */
#define LAPIC_ICRLO 0x300               /* writing here fires the IPI */
#define LAPIC_ICRHI 0x310               /* destination APIC ID in bits 24..31 */
#define SVR_ENABLE  0x100               /* SVR bit 8: software-enable the LAPIC */
#define ICR_INIT    0x4500              /* delivery=INIT(5), level=assert */
#define ICR_STARTUP 0x4600              /* delivery=STARTUP(6), level=assert; | vec */
#define ICR_PENDING (1u << 12)          /* ICR-low delivery-status: 1 while sending */

int smp_cpu_count = 1;                  /* the BSP counts as one */

static volatile uint32_t *lapic;        /* set in smp_init via the HHDM */

static inline uint32_t lapic_rd(uint32_t off) { return lapic[off / 4]; }
static inline void     lapic_wr(uint32_t off, uint32_t v) { lapic[off / 4] = v; }

/* The APIC id of the core executing right now (sched_getcpu, M1246). ring-3
 * tasks run on the BSP, so this is the BSP's id for syscall callers. */
int smp_current_cpu(void) { return lapic ? (int)((lapic_rd(LAPIC_ID) >> 24) & 0xFF) : 0; }

static inline uint64_t rdmsr(uint32_t m) {
    uint32_t a, d; __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(m));
    return ((uint64_t)d << 32) | a;
}
static inline void wrmsr(uint32_t m, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t read_cr3(void) {
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}

/* Software-enable the local APIC on whichever core calls this (the LAPIC is
 * per-CPU). Set the xAPIC global-enable bit in the APIC_BASE MSR, drop the task
 * priority to accept all interrupts, then set the spurious-vector register's
 * enable bit (the actual on switch). */
void lapic_enable_this_cpu(void) {
    wrmsr(0x1B, rdmsr(0x1B) | (1ull << 11));   /* IA32_APIC_BASE.EN (keep base) */
    lapic_wr(LAPIC_TPR, 0);
    lapic_wr(LAPIC_SVR, SVR_ENABLE | 0xFF);    /* enable + spurious vector 0xFF */
}

/* PIT channel-2 one-shot busy delay — independent of interrupts and the
 * scheduler, which is what we need this early. Channel 2's gate and output are
 * wired to port 0x61 (bit 0 = gate, bit 5 = output). Program a count, pulse the
 * gate, and spin until the output goes high at terminal count. Max ~54 ms/call
 * (16-bit count at 1.193182 MHz); our longest single wait is 10 ms. */
static void pit_udelay(uint32_t us) {
    uint32_t count = (uint32_t)(((uint64_t)us * 1193182u) / 1000000u);
    if (count == 0) count = 1;
    if (count > 0xFFFF) count = 0xFFFF;
    uint8_t p = inb(0x61) & 0xFC;          /* speaker off (bit1), gate low (bit0) */
    outb(0x61, p);
    outb(0x43, 0xB0);                      /* ch2, lobyte/hibyte, mode 0, binary */
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));
    outb(0x61, (uint8_t)(p | 0x01));       /* gate high -> start the countdown */
    while (!(inb(0x61) & 0x20))            /* wait for OUT (bit 5) = terminal count */
        __asm__ volatile("pause");
}

void lapic_eoi(void) { if (lapic) lapic_wr(0x0B0, 0); }   /* ack an interrupt */

/* ---- parallel job pool (M1198) -----------------------------------------
 * The BSP dispatches a batch of pure-compute chunks; the APs (and the BSP
 * itself) pull from the queue and run them in parallel. A tiny spinlock guards
 * only the claim/setup — the job function runs OUTSIDE the lock, so the cores
 * execute concurrently. Job functions must be pure compute: no kmalloc, FS, or
 * other unsynchronised kernel state (the rest of the kernel is still BSP-only).
 */
#define SMP_MAXJOB 64
struct smp_job { smp_fn fn; int lo, hi; void *ctx; };
static struct smp_job sj[SMP_MAXJOB];
static volatile int sj_n, sj_next, sj_done, sj_lock;
static volatile unsigned cpu_jobs[MAX_CPUS];   /* jobs run, indexed by APIC id */

static void slock(void)   { while (__atomic_exchange_n(&sj_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile("pause"); }
static void sunlock(void) { __atomic_store_n(&sj_lock, 0, __ATOMIC_RELEASE); }

/* Claim and run one job; returns 1 if a job ran, 0 if the queue is drained. */
static int smp_run_one(void) {
    slock();
    int i = (sj_next < sj_n) ? sj_next++ : -1;
    sunlock();
    if (i < 0) return 0;
    sj[i].fn(sj[i].lo, sj[i].hi, sj[i].ctx);                   /* parallel: no lock held */
    uint32_t id = (lapic_rd(LAPIC_ID) >> 24) & 0xFF;
    __atomic_add_fetch(&cpu_jobs[id & (MAX_CPUS - 1)], 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&sj_done, 1, __ATOMIC_SEQ_CST);
    return 1;
}

/* Wake every AP (all-but-self shorthand) with a fixed IPI at vector 0x40. Also
 * used by smpthread.c (M1530) to nudge a freshly-spawned thread's target core
 * out of hlt promptly, the same signal smp_parallel_for already relies on. */
void smp_wake_aps(void) {
    if (!lapic) return;
    lapic_wr(LAPIC_ICRLO, 0x40 | (1u << 14) | (3u << 18));     /* fixed, assert, all-but-self */
    while (lapic_rd(LAPIC_ICRLO) & ICR_PENDING) __asm__ volatile("pause");
}

/* Run fn over [0, n) split into chunks across all online cores, in parallel.
 * The BSP participates and returns only once every chunk has completed. */
void smp_parallel_for(int n, smp_fn fn, void *ctx) {
    if (n <= 0 || !fn) return;
    int nc = __atomic_load_n(&smp_cpu_count, __ATOMIC_SEQ_CST);
    int chunks = nc * 4;                                       /* finer than cores -> load-balances */
    if (chunks > SMP_MAXJOB) chunks = SMP_MAXJOB;
    if (chunks < 1) chunks = 1;
    int per = (n + chunks - 1) / chunks;

    slock();
    sj_next = 0; sj_done = 0; sj_n = 0;
    int k = 0;
    for (int lo = 0; lo < n && k < SMP_MAXJOB; lo += per) {
        int hi = lo + per; if (hi > n) hi = n;
        sj[k].fn = fn; sj[k].lo = lo; sj[k].hi = hi; sj[k].ctx = ctx; k++;
    }
    sj_n = k;
    sunlock();

    smp_wake_aps();                                            /* kick the idle APs */
    while (smp_run_one()) ;                                    /* the BSP works too */
    while (__atomic_load_n(&sj_done, __ATOMIC_SEQ_CST) < k)    /* join */
        __asm__ volatile("pause");
}

/* AP entry, reached from ap_trampoline.asm once the core is in 64-bit long mode
 * on its own stack with the kernel page tables loaded. Adopt the kernel GDT+IDT
 * (so this core can take interrupts), announce ourselves, then idle: drain any
 * dispatched jobs, else sleep in hlt until the next wake IPI. Power-friendly —
 * an idle core is halted, not spinning (which would peg a host CPU). */
void ap_main(void) {
    lapic_enable_this_cpu();
    gdt_load_ap();                       /* kernel GDT: KERNEL_CS valid on a trap */
    idt_load();                          /* kernel IDT: the wake IPI + exceptions */
    __atomic_add_fetch(&smp_cpu_count, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("sti");             /* accept the wake IPI */
    for (;;) {
        while (smp_run_one()) ;          /* run everything currently queued */
        smpthread_ap_tick();             /* run any real kernel threads pinned to this core (M1530) */
        __asm__ volatile("cli");         /* re-check with interrupts off (no lost wakeup) */
        if (__atomic_load_n(&sj_next, __ATOMIC_SEQ_CST) >= __atomic_load_n(&sj_n, __ATOMIC_SEQ_CST))
            __asm__ volatile("sti; hlt");   /* sleep until an IPI; sti;hlt is atomic */
        else
            __asm__ volatile("sti");
    }
}

/* Boot self-test (M1198): sum 0..N in parallel across the cores and check it
 * against the closed form, proving the pool actually executes work on the APs.
 * Records how many cores participated for the [smp] log + /proc/cpuinfo. */
int smp_selftest_cores;     /* cores that ran >=1 chunk */
int smp_selftest_ok;        /* 1 = the parallel sum matched */

static volatile uint64_t st_partial[MAX_CPUS];
static void sum_chunk(int lo, int hi, void *ctx) {
    (void)ctx;
    uint64_t s = 0;
    for (int i = lo; i < hi; i++) s += (unsigned)i;
    uint32_t id = (lapic_rd(LAPIC_ID) >> 24) & 0xFF;
    __atomic_add_fetch(&st_partial[id & (MAX_CPUS - 1)], s, __ATOMIC_SEQ_CST);
}
static void smp_selftest(void) {
    const int N = 4000000;
    smp_parallel_for(N, sum_chunk, 0);
    uint64_t total = 0; int cores = 0;
    for (int i = 0; i < MAX_CPUS; i++) { total += st_partial[i]; if (st_partial[i]) cores++; }
    uint64_t expect = (uint64_t)N * (N - 1) / 2;
    smp_selftest_cores = cores;
    smp_selftest_ok = (total == expect);
    kprintf("[smp] parallel self-test: sum(0..%d) %s on %d core(s)\n",
            N, smp_selftest_ok ? "OK" : "MISMATCH", cores);
}

/* Send one IPI to `apic_id` and wait for the LAPIC to report it delivered. */
static void icr_send(uint8_t apic_id, uint32_t lo) {
    lapic_wr(LAPIC_ICRHI, (uint32_t)apic_id << 24);   /* destination */
    lapic_wr(LAPIC_ICRLO, lo);                        /* fire */
    while (lapic_rd(LAPIC_ICRLO) & ICR_PENDING)
        __asm__ volatile("pause");
}

/* Bring up a single AP via the Intel INIT–SIPI–SIPI sequence. Returns the
 * online count observed after waiting (the caller compares it to detect a core
 * that came up). */
static void ap_start_one(uint8_t apic_id) {
    int before = __atomic_load_n(&smp_cpu_count, __ATOMIC_SEQ_CST);

    icr_send(apic_id, ICR_INIT);                       /* INIT assert */
    pit_udelay(10000);                                 /* 10 ms */
    icr_send(apic_id, ICR_STARTUP | (TRAMP_PHYS >> 12)); /* SIPI #1, vector 0x08 */
    pit_udelay(200);                                   /* ~200 us */
    icr_send(apic_id, ICR_STARTUP | (TRAMP_PHYS >> 12)); /* SIPI #2 (ignored if up) */

    /* Wait up to ~100 ms for ap_main to bump the counter. */
    for (int i = 0; i < 100; i++) {
        if (__atomic_load_n(&smp_cpu_count, __ATOMIC_SEQ_CST) > before) return;
        pit_udelay(1000);
    }
    kprintf("[smp] AP apic=%u did not come up\n", apic_id);
}

void smp_init(void) {
    /* The HHDM only covers RAM, but the LAPIC MMIO sits at ~4 GiB (above RAM),
     * so its HHDM virtual address is unmapped — map that one page explicitly,
     * cache-disabled (PCD), into the kernel PML4 (which the APs then inherit). */
    lapic = (volatile uint32_t *)hhdm(LAPIC_PHYS);
    vmm_map((uint64_t)lapic, LAPIC_PHYS, PTE_WRITABLE | PTE_PCD);
    lapic_enable_this_cpu();                           /* BSP's own LAPIC */

    uint8_t ids[MAX_CPUS];
    int n = acpi_madt_lapics(ids, MAX_CPUS);
    uint32_t bsp = (lapic_rd(LAPIC_ID) >> 24) & 0xFF;
    if (n <= 1) { kprintf("[smp] uniprocessor (MADT lists %d CPU%s)\n",
                          n < 1 ? 1 : n, n == 1 ? "" : "s"); return; }

    /* Stage the trampoline at physical 0x8000 and fill its parameter block. The
     * page is in the reserved low region (pmm never hands it out) and is
     * identity-mapped by the kernel PML4 the AP loads. */
    extern char ap_tramp_start[], ap_tramp_end[];
    __builtin_memcpy(hhdm(TRAMP_PHYS), ap_tramp_start,
                     (uint64_t)(ap_tramp_end - ap_tramp_start));
    volatile uint64_t *param = (volatile uint64_t *)hhdm(AP_PARAM);
    param[0] = read_cr3() & PTE_ADDR_MASK;             /* kernel PML4 -> CR3 */
    param[2] = (uint64_t)&ap_main;                     /* 64-bit C entry */

    /* An AP's stack is allocated ONCE here and used for the rest of the kernel's
     * life -- originally 16 KiB, sized only for the idle loop + the trivial boot
     * self-test below. M1528 made smp_parallel_for's first real caller (TLS
     * chain-link verification) dispatch full ECDSA point-arithmetic (bignum
     * mul/modexp, a deep call chain) onto whichever core picks up a chunk,
     * including an AP -- found via an in-guest crash (a real KERNEL STACK
     * OVERFLOW booting against the live internet) that 16 KiB isn't remotely
     * enough for that, matching the exact reason task.c's task_create_stack
     * gives bignum/RSA/ECDSA-heavy kernel tasks 256 KiB instead of the 16 KiB
     * default (M1491/M1520). Now sized the same, and via kstack_alloc (M1495)
     * instead of a raw pmm_alloc_contiguous, so an AP that ever DOES overflow
     * this faults cleanly on a guard page instead of corrupting whatever
     * physical memory happened to sit past a bare HHDM allocation. */
#define AP_STACK_SIZE (256 * 1024)
    for (int i = 0; i < n; i++) {
        if (ids[i] == bsp) continue;                   /* don't IPI ourselves */
        void *stk = kstack_alloc(AP_STACK_SIZE);
        if (!stk) { kprintf("[smp] no stack for apic=%u\n", ids[i]); continue; }
        param[1] = (uint64_t)stk + AP_STACK_SIZE;      /* stack top */
        ap_start_one(ids[i]);
    }

    kprintf("[smp] %d of %d CPUs online (BSP apic=%u)\n",
            __atomic_load_n(&smp_cpu_count, __ATOMIC_SEQ_CST), n, bsp);

    smp_selftest();   /* prove the cores actually run work in parallel */
}
