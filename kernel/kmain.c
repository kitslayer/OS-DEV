/*
 * kmain.c — kernel entry from the assembly trampoline (long mode, C land).
 *
 * Brings up every subsystem in order, runs a couple of demos (preemption,
 * per-process isolation), then hands the screen to the windowing desktop, which
 * hosts real ring-3 userspace programs as windows.
 */
#include "console.h"
#include "vga.h"
#include "gdt.h"
#include "interrupts.h"
#include "timer.h"
#include "keyboard.h"
#include "serial.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "task.h"
#include "fat32.h"
#include "vfs.h"
#include "pci.h"
#include "net.h"
#include "fbcon.h"
#include "mouse.h"
#include "usb.h"
#include "speaker.h"
#include "desktop.h"
#include <stdint.h>

/* --- preemption demo: a worker that never yields -------------------------- */
static volatile uint64_t spin_count;
static volatile int       demo_stop;
static volatile int       worker_done;

static void spin_worker(void) {
    while (!demo_stop)
        spin_count++;          /* tight loop, NEVER calls task_yield() */
    worker_done = 1;
    task_exit();
}

/* --- per-process isolation demo --- */
static volatile int iso_live;

static void iso_worker(void) {
    int id = task_current_id();
    volatile int *v = (volatile int *)0x40000000;   /* same vaddr in every proc */
    *v = id * 100;                                   /* write into MY private page */
    for (int i = 0; i < 3; i++) {
        kprintf("    [proc %d] *0x40000000 = %d (mine=%d)%s\n",
                id, *v, id * 100, (*v == id * 100) ? "  isolated" : "  LEAK!");
        task_yield();
    }
    iso_live--;
    task_exit();
}

static void isolation_demo(void) {
    kprintf("[demo] memory isolation: 3 processes each map a PRIVATE page at the\n");
    kprintf("       same address 0x40000000 and write their id; none see another's:\n");
    iso_live = 3;
    for (int i = 0; i < 3; i++) {
        uint64_t cr3 = vmm_create_address_space();
        vmm_map_to(cr3, 0x40000000, pmm_alloc_frame(), PTE_WRITABLE | PTE_USER);
        task_create(iso_worker, cr3, 0);
    }
    while (iso_live > 0)
        task_yield();
    kprintf("[demo] all isolated => each process has its own address space.\n\n");
}

static void preemption_demo(void) {
    kprintf("[demo] preemption: spawning a worker stuck in an infinite,\n");
    kprintf("       never-yielding loop. Under cooperative scheduling this\n");
    kprintf("       would freeze main forever. With preemption, both run:\n");
    task_create(spin_worker, 0, 0);
    for (int i = 0; i < 4; i++) {
        uint64_t target = timer_ticks() + 40;     /* ~0.4s of wall time */
        while (timer_ticks() < target) { }         /* main also gets preempted */
        kprintf("    [main] alive at %lu ticks  |  worker spin_count = %lu\n",
                timer_ticks(), spin_count);
    }
    demo_stop = 1;
    while (!worker_done)
        task_yield();
    kprintf("[demo] worker counted to %lu while main kept printing"
            " => preemption works.\n\n", spin_count);
}

void kmain(uint64_t mb_info) {
    console_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("OS-DEV  -  x86_64 kernel\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("========================\n\n");

    gdt_init();
    interrupts_init();
    timer_init(100);
    keyboard_init();
    interrupts_enable();
    serial_enable_rx_irq();        /* let the serial line feed keyboard input */
    pmm_init(mb_info);
    vmm_init();
    kheap_init();
    sched_init();

    /* Switch the console to the framebuffer: from here, all output renders
     * graphically with a real font. */
    if (fbcon_init() == 0) {
        console_enable_gfx();
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);   /* (no-op in gfx, harmless) */
    }

    kprintf("OS-DEV  -  x86_64 kernel  (graphical console)\n");
    kprintf("=============================================\n\n");
    kprintf("[ ok ] full bring-up complete (%lu MiB RAM).\n\n",
            pmm_total_bytes() / (1024 * 1024));

    preemption_demo();
    isolation_demo();

    kprintf("[ ok ] PCI devices on the bus:\n");
    pci_enumerate();
    kprintf("\n");

    net_demo();

    /* Mount the FAT32 disk and show it works from the kernel side. */
    if (fat32_mount() == 0) {
        kprintf("[ ok ] mounted FAT32 volume (ATA primary master).\n\n");
        vfs_dirent ents[32];
        int n = vfs_list(ents, 32);
        kprintf("  / contains %d file(s):\n", n);
        for (int i = 0; i < n; i++)
            kprintf("    %s  (%u bytes)\n", ents[i].name, ents[i].size);
        char fbuf[256];
        long r = vfs_read("README.TXT", fbuf, sizeof(fbuf) - 1);
        if (r > 0) {
            fbuf[r] = '\0';
            kprintf("  --- README.TXT ---\n%s  -------------------\n\n", fbuf);
        }
    } else {
        kprintf("[warn] no FAT32 disk found (run with a disk image).\n\n");
    }

    /* Prefer the USB tablet (absolute pointer, tracks the host 1:1); fall back
     * to the relative PS/2 mouse if there's no tablet. */
    if (usb_tablet_init() == 0) {
        kprintf("[ ok ] USB tablet active (absolute pointer).\n");
    } else {
        mouse_init();
        kprintf("[ ok ] PS/2 mouse on IRQ12 (relative fallback).\n");
    }
    kprintf("[main] launching the desktop environment...\n");
    speaker_chime();              /* a little startup arpeggio */
    desktop_run();
}
