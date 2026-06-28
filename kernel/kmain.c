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
#include "acpi.h"
#include "hpet.h"   /* high-resolution HPET clocksource (M1273) */
#include "smp.h"
#include "multiboot.h"
#include "gdbstub.h"
#include "random.h"
#include "vdso.h"
#include "measure.h"
#include "task.h"
#include "fat32.h"
#include "vfs.h"
#include "partition.h"
#include "blockdev.h"
#include "ext2.h"   /* ext2_set_clock (M1175) */
#include "rtc.h"    /* rtc_unix (M1175) */
#include "dm.h"
#include "ata.h"
#include "pci.h"
#include "ahci.h"
#include "virtio_blk.h"
#include "virtio_rng.h"
#include "virtio_console.h"
#include "bpf.h"           /* bpf_jit_selftest (M1290) */
#include "nvme.h"
#include "floppy.h"
#include "virtio_net.h"
#include "virtio_gpu.h"
#include "svga.h"
#include "net.h"
#include "fbcon.h"
#include "fb.h"            /* fb_init_mb: consume a Multiboot/GRUB framebuffer (M1292) */
#include "string.h"        /* memset for the Multiboot2 shim (M1293) */
#include "mouse.h"
#include "usb.h"
#include "usb_storage.h"
#include "usb_kbd.h"
#include "ehci.h"
#include "xhci.h"
#include "speaker.h"
#include "audio.h"
#include "hda.h"
#include "desktop.h"
#include <stdint.h>

extern void fpu_init(void);      /* kernel/asm/fpu.asm: enable x87 + SSE */

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
        vmm_map_to(cr3, 0x40000000, pmm_alloc_frame(), PTE_WRITABLE | PTE_USER | PTE_NX);
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

/* substring search (the kernel libc has no strstr) — for the gdbstub cmdline gate */
static int cmdline_has(const char *hay, const char *needle) {
    for (const char *p = hay; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

/* CPU security hardening (M1269): enable SMEP (CR4 bit 20 — the kernel #PFs if it
 * ever tries to EXECUTE a ring-3 page) and UMIP (CR4 bit 11 — ring-3
 * SGDT/SIDT/SLDT/STR/SMSW #GP, closing those kernel-address info leaks), each
 * gated on CPUID.7:0 support. SMAP (CR4 bit 21) is deliberately NOT set: the
 * kernel reads/writes user buffers directly (syscall args) without stac/clac,
 * which SMAP would fault on. BSP only — ring-3 code runs on the BSP. */
static void cpu_harden(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0u), "c"(0u));
    if (a < 7) { kprintf("[cpu] CPUID leaf 7 unavailable; no SMEP/UMIP\n"); return; }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7u), "c"(0u));
    int have_smep = (b >> 7) & 1;    /* CPUID.(EAX=7,ECX=0).EBX[7] */
    int have_umip = (c >> 2) & 1;    /* CPUID.(EAX=7,ECX=0).ECX[2] */
    uint64_t cr4; __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (have_smep) cr4 |= (1ull << 20);
    if (have_umip) cr4 |= (1ull << 11);
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    uint64_t now; __asm__ volatile("mov %%cr4, %0" : "=r"(now));
    kprintf("[ ok ] CPU hardening: SMEP=%d UMIP=%d (CR4=%lx)\n",
            (int)((now >> 20) & 1), (int)((now >> 11) & 1), (unsigned long)now);
}

/* Multiboot2 -> Multiboot1 shim (M1293, bare-metal graphics). GRUB booted via
 * `multiboot2` passes a TAG LIST, not the Multiboot1 struct the kernel reads —
 * and, unlike Multiboot1, it reliably provides a framebuffer. Walk the tags and
 * fill an MB1-format struct (memory map converted entry-by-entry + the
 * framebuffer) so pmm_init / fb consumption / everything downstream work
 * unchanged. The static buffers live in the (low, identity-mapped) kernel image,
 * so their addresses fit MB1's u32 mmap_addr. */
#define MULTIBOOT2_MAGIC 0x36d76289u
static struct multiboot_info       mb1_shim;
static struct multiboot_mmap_entry mb1_shim_mmap[96];
static uint64_t mb2_to_mb1(uint64_t mb2) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)mb2;
    uint32_t total = *(const uint32_t *)p;          /* total_size, then reserved, then tags */
    memset(&mb1_shim, 0, sizeof mb1_shim);
    int nm = 0;
    for (uint32_t off = 8; off + 8 <= total; ) {
        const uint8_t *tag = p + off;
        uint32_t type = *(const uint32_t *)tag;
        uint32_t size = *(const uint32_t *)(tag + 4);
        if (type == 0) break;                                  /* end tag */
        if (type == 4) {                                       /* basic meminfo */
            mb1_shim.mem_lower = *(const uint32_t *)(tag + 8);
            mb1_shim.mem_upper = *(const uint32_t *)(tag + 12);
            mb1_shim.flags |= MULTIBOOT_FLAG_MEM;
        } else if (type == 6) {                                /* memory map */
            uint32_t esz = *(const uint32_t *)(tag + 8);
            if (esz >= 24)
                for (const uint8_t *e = tag + 16; e + esz <= tag + size && nm < 96; e += esz) {
                    mb1_shim_mmap[nm].size = 20;               /* MB1 entry: size excludes itself */
                    mb1_shim_mmap[nm].addr = *(const uint64_t *)e;
                    mb1_shim_mmap[nm].len  = *(const uint64_t *)(e + 8);
                    mb1_shim_mmap[nm].type = *(const uint32_t *)(e + 16);
                    nm++;
                }
        } else if (type == 8) {                                /* framebuffer */
            mb1_shim.framebuffer_addr   = *(const uint64_t *)(tag + 8);
            mb1_shim.framebuffer_pitch  = *(const uint32_t *)(tag + 16);
            mb1_shim.framebuffer_width  = *(const uint32_t *)(tag + 20);
            mb1_shim.framebuffer_height = *(const uint32_t *)(tag + 24);
            mb1_shim.framebuffer_bpp    = *(const uint8_t  *)(tag + 28);
            mb1_shim.framebuffer_type   = *(const uint8_t  *)(tag + 29);
            mb1_shim.flags |= MULTIBOOT_FLAG_FB;
        }
        off += (size + 7u) & ~7u;                              /* tags are padded to 8 bytes */
    }
    if (nm) {
        mb1_shim.mmap_addr   = (uint32_t)(uintptr_t)mb1_shim_mmap;
        mb1_shim.mmap_length = (uint32_t)(nm * (int)sizeof(struct multiboot_mmap_entry));
        mb1_shim.flags |= MULTIBOOT_FLAG_MMAP;
    }
    return (uint64_t)(uintptr_t)&mb1_shim;
}

/* ---- deliberate kernel-stack-overflow test (M1498) ----------------------------
 * Gated behind `-append kstackover`: spawn a kernel task that recurses until it
 * runs off its guarded stack, exercising the WHOLE M1495/M1496 path end-to-end —
 * the guard-page #PF, the "KERNEL STACK OVERFLOW" diagnosis, and a panic backtrace
 * that has to walk a high-VA stack. Normal boots (no flag) never touch this. */
static volatile int g_kstack_overflow_test;

static int __attribute__((noinline)) kstack_blow(int d) {
    volatile char buf[512];
    for (int i = 0; i < 512; i++) buf[i] = (char)(d + i);    /* real per-frame stack use */
    int r = 0;
    if (g_kstack_overflow_test) r = kstack_blow(d + 1);      /* volatile guard: recurse, but the compiler can't fold it to infinite */
    return r + buf[(unsigned)d & 511];                       /* use buf AFTER the call: not a tail call */
}
static void kstack_overflow_task(void) {
    kprintf("[kstacktest] deliberately overflowing this task's kernel stack...\n");
    volatile int sink = kstack_blow(0);
    (void)sink;                                              /* unreachable: the recursion faults into the guard page first */
}

void kmain(uint64_t mb_info, uint64_t magic) {
    console_init();

    /* GRUB `multiboot2` hands a tag list, not the Multiboot1 struct; convert it
     * up front so the rest of boot (pmm, framebuffer, ...) is identical. (M1293) */
    if (magic == MULTIBOOT2_MAGIC)
        mb_info = mb2_to_mb1(mb_info);

    /* Detect `-append gdbstub` NOW, before pmm_init/kheap_init can allocate over
     * the multiboot command-line string (which QEMU places in high usable RAM).
     * Just sets a flag; the actual break into the stub happens after smp_init. */
    {
        struct multiboot_info *mbi = (struct multiboot_info *)(uintptr_t)mb_info;
        const char *cl = ((mbi->flags & (1u << 2)) && mbi->cmdline) ? (const char *)(uintptr_t)mbi->cmdline : "";
        if (cmdline_has(cl, "gdbstub"))    gdbstub_arm();
        if (cmdline_has(cl, "kstackover")) g_kstack_overflow_test = 1;   /* deliberate guarded-stack overflow test (M1498) */
    }

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("OS-DEV  -  x86_64 kernel\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("========================\n\n");

    gdt_init();
    interrupts_init();
    fpu_init();                    /* enable x87 + SSE so userspace can use floating point */
    cpu_harden();                  /* SMEP + UMIP: kernel can't run ring-3 pages; ring-3 can't SGDT/etc (M1269) */
    timer_init(100);
    keyboard_init();
    interrupts_enable();
    serial_enable_rx_irq();        /* let the serial line feed keyboard input */
    pmm_init(mb_info);
    vmm_init();
    kheap_init();
    acpi_init();                   /* find the ACPI tables for clean poweroff/reboot (uses hhdm) */
    hpet_init();                   /* high-resolution clocksource via the ACPI HPET table (M1273) */
    smp_init();                    /* enable the LAPIC + bring the other cores online (M1197) */

    /* GDB remote-serial stub (M1204): if `-append gdbstub` was seen (detected at
     * the top of kmain, before allocations could clobber the cmdline), break into
     * the stub here so a host gdb can attach over COM2 — kmain int3's and the #BP
     * handler runs the RSP loop. `continue`/`detach` from gdb resumes the boot. */
    if (gdbstub_armed()) {
        kprintf("[gdbstub] waiting for gdb on COM2 (target remote :PORT); 'continue' resumes boot\n");
        __asm__ volatile("int3");
    }

    random_init();                 /* seed the CSPRNG from RDSEED/RDRAND (TSC fallback) */
    vdso_init();                   /* alloc the vDSO time page + seed the wall clock from the RTC (M1111) */

    /* Measured boot (M1096): fold the kernel's read-only image (.text+.rodata,
     * which includes every embedded app ELF) into PCR0 before anything runs.
     * Each app then extends PCR1 at spawn, building a replayable attestation log. */
    {
        extern char _kimage_start[], _kimage_end[];
        measure_init();
        measure_extend(PCR_KERNEL, _kimage_start, (uint64_t)(_kimage_end - _kimage_start), "kernel");
    }

    /* W^X: split the boot huge-pages over the kernel image into 4 KiB pages and
     * tighten them — .text read-only+executable, .rodata read-only+NX, .data/.bss/
     * stack writable+NX (the eBPF/module .jitexec scratch stays RWX). Done here:
     * pmm/vmm are up and we are still single-threaded in the kernel address space. */
    vmm_harden_kernel();
    kstack_selftest();             /* prove the guarded task-stack guard pages are really unmapped (M1495) */

    sched_init();

    /* On real hardware / a GRUB ISO, the bootloader honors our Multiboot header's
     * video request and reports a linear framebuffer here; use it. (QEMU -kernel
     * honors the request too, so this path is exercised under QEMU as well.) If
     * absent/unusable, fbcon_init falls back to the Bochs std-VGA mode-set. (M1292) */
    {
        struct multiboot_info *fbi = (struct multiboot_info *)(uintptr_t)mb_info;
        if ((fbi->flags & MULTIBOOT_FLAG_FB) && fbi->framebuffer_type == 1 &&
            fb_init_mb(fbi->framebuffer_addr, (int)fbi->framebuffer_width,
                       (int)fbi->framebuffer_height, (int)fbi->framebuffer_pitch,
                       fbi->framebuffer_bpp) == 0)
            kprintf("[ ok ] Multiboot framebuffer %ux%u %u-bpp @ %p -- bare-metal graphics path\n",
                    fbi->framebuffer_width, fbi->framebuffer_height, fbi->framebuffer_bpp,
                    (void *)(uintptr_t)fbi->framebuffer_addr);
    }

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

    if (g_kstack_overflow_test)            /* `-append kstackover`: prove the guarded-stack fault path end-to-end (M1498) */
        task_create(kstack_overflow_task, 0, 0);

    preemption_demo();
    isolation_demo();

    kprintf("[ ok ] PCI devices on the bus:\n");
    pci_enumerate();
    kprintf("\n");

    audio_init();     /* bring up audio: HDA if present, else AC'97 (no-op if neither) */
    kprintf("[ ok ] audio output: %s\n", audio_name());
    hda_selftest();   /* if HDA is active: prove the stream DMA advances (no-op otherwise) */

    /* Bring up virtio-net (the paravirtual NIC) and log its MAC if present — a
     * no-op if no virtio network device is attached. net_demo()'s nic_init()
     * binds whichever NIC is present (e1000, then rtl8139, then this); the call
     * here is idempotent and just surfaces the MAC before the stack demo runs. */
    virtio_net_init();
    virtio_net_selftest();

    bpf_jit_selftest();            /* prove the eBPF JIT matches the interpreter (M1290) */

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

    /* Enumerate ALL legacy ATA drives (primary/secondary bus x master/slave) and
     * parse each one's MBR/GPT partition table, logging every partition as a
     * (drive, type, start-LBA, sectors) volume. This is purely additive: the boot
     * mount above still reads the bare FAT32 off drive 0 at LBA 0. With no extra
     * disks attached, only drive 0 is present and (being a bare FS) reports no
     * partition table — a clean no-op. */
    partition_enumerate();
    kprintf("\n");

    /* Prove the ADDITIVE bus-master IDE DMA path (kernel/ata.c) against the boot
     * disk: the boot disk sits on the PIIX3 IDE controller (bus-master capable),
     * so this DMA-reads a few of its sectors and compares them byte-for-byte
     * against a PIO read of the same sectors, logging "DMA==PIO OK" per sector.
     * This is purely additive: fat32/vfs/boot still use the PIO ata_read/write
     * above; the DMA path is a separate, proven-identical capability. A clean
     * no-op (logs "DMA unavailable") if no PIIX3 BMIDE controller is present. */
    ata_dma_selftest();

    /* Bring up AHCI/SATA as an ADDITIONAL storage driver (the boot disk above
     * stays on legacy ATA). No-op if no AHCI HBA + disk is attached. The
     * self-test reads real sectors off the AHCI disk and logs their bytes. */
    ahci_init();
    ahci_selftest();

    /* Bring up virtio-blk (the paravirtual "fast VM disk") as ANOTHER additional
     * storage driver — boot still uses legacy ATA above. No-op if no virtio block
     * device is attached. The self-test reads real sectors off it (and does a
     * write round-trip) and logs their bytes. */
    virtio_blk_init();
    virtio_blk_selftest();

    /* Bring up virtio-rng (the paravirtual hardware entropy source) — the
     * simplest virtio device: hand it a buffer over a virtqueue and it DMAs
     * random bytes in. No-op if no virtio-rng device is attached; the self-test
     * draws two batches and logs them to prove real entropy moved. */
    virtio_rng_init();
    virtio_rng_selftest();

    /* Bring up virtio-console (a paravirtual serial port to the host): the guest
     * writes bytes to the transmit virtqueue and they land in whatever chardev
     * the hypervisor wired up. No-op if no virtio-console device is attached;
     * the self-test emits one line so the host sink + serial log show it. */
    virtio_console_init();
    virtio_console_selftest();

    /* Bring up NVMe (modern PCIe storage) as YET ANOTHER additional storage
     * driver — boot still uses legacy ATA above. No-op if no NVMe controller is
     * attached. The self-test identifies namespace 1, reads real sectors off it
     * (and does a write round-trip) and logs their bytes/checksum. */
    nvme_init();
    nvme_selftest();

    /* Bring up the legacy floppy controller (82077AA) as ANOTHER additional
     * block device — boot still uses legacy ATA above. Unlike every other DMA
     * driver here (which bus-master over PCI), the floppy moves its data through
     * the legacy 8237 ISA DMA controller (channel 2 + a low-RAM bounce buffer
     * that must not cross a 64 KiB boundary). No-op if no FDC/diskette is
     * attached. The self-test resets+recalibrates the controller and ISA-DMA
     * reads real sectors off the diskette, logging their bytes/checksum. */
    floppy_init();
    floppy_selftest();

    /* Bring up virtio-gpu (the modern paravirtual 2D GPU) as an ADDITIONAL
     * display device — the boot display stays on the linear framebuffer
     * (fb.c/bochs_vbe.c) above. No-op if no virtio-gpu is attached. The self-test
     * runs the full present cycle (CREATE_2D + ATTACH_BACKING + SET_SCANOUT done
     * at init; then TRANSFER_TO_HOST_2D + RESOURCE_FLUSH of a test pattern) and
     * asserts every command returned OK — the headless proof, like hda_selftest. */
    virtio_gpu_init();
    virtio_gpu_selftest();

    /* Bring up VMware SVGA-II (PCI 0x15AD:0x0405) as YET ANOTHER additional
     * display device — the boot display stays on the linear framebuffer
     * (fb.c/bochs_vbe.c) above. No-op if no vmware-svga is attached. Driven
     * through an I/O-port index/value register file + a linear framebuffer
     * (BAR1) + a command FIFO (BAR2). The self-test confirms SVGA_ID_2, sets a
     * mode, writes a colour-band test pattern to the framebuffer, emits an
     * SVGA_CMD_UPDATE into the FIFO + syncs, and reads registers back to
     * confirm — the headless proof, like virtio_gpu_selftest. */
    svga_init();
    svga_selftest();

    /* Prefer the USB tablet (absolute pointer, tracks the host 1:1); fall back
     * to the relative PS/2 mouse if there's no tablet. */
    if (usb_tablet_init() == 0) {
        kprintf("[ ok ] USB tablet active (absolute pointer).\n");
    } else {
        mouse_init();
        kprintf("[ ok ] PS/2 mouse on IRQ12 (relative fallback).\n");
    }

    /* Bring up USB mass-storage (a USB flash disk) as an ADDITIONAL block device,
     * sharing the one UHCI controller with the tablet above — boot still uses
     * legacy ATA. No-op if no USB mass-storage device is attached (the tablet
     * path is unaffected). The self-test READ-CAPACITYs it and reads real sectors
     * off it via Bulk-Only Transport + SCSI, logging their bytes/checksum. */
    usb_storage_init();
    usb_storage_selftest();

    /* Generic block-device browsing across EVERY storage driver brought up above.
     * Each driver (ATA/AHCI/virtio-blk/NVMe/USB-storage) only SELF-TESTED its raw
     * sectors; this registers every present device behind one uniform read
     * interface (kernel/blockdev.c) and then, for each, MOUNTS any FAT32 volume it
     * carries (bare at LBA 0, or inside an MBR/GPT partition) READ-ONLY and LISTS
     * its root directory — proving the disks are genuinely browsable, not just
     * readable. Purely additive + read-only: the boot FAT32 mount (ATA primary
     * master, LBA 0) above and fat32.c/vfs.c are untouched. A clean no-op listing
     * if a device carries no FAT32. */
    ext2_set_clock(rtc_unix);      /* real inode timestamps on ext2 writes (M1175) */
    blockdev_enumerate();
    blockdev_selftest();           /* verify the write vtable + buffer-cache coherence (M1095) */
    dm_selftest();                 /* RAID-1 mirror self-test, iff 2 non-boot writable disks (M1157) */
    kprintf("\n");

    /* Bring up a USB HID boot keyboard, sharing the one UHCI controller with the
     * tablet + mass-storage above (skipping the tablet's port, using the shared
     * USB address allocator) — the PS/2 keyboard above stays the primary input.
     * No-op if no USB keyboard is attached (PS/2 + tablet + storage unaffected).
     * The self-test reports the enumerated HID boot keyboard + decodes any
     * keystroke injected around boot, and the desktop polls it alongside the
     * tablet so USB keystrokes reach the shell/apps like PS/2 ones. */
    usb_kbd_init();
    usb_kbd_selftest();

    /* Bring up an EHCI (USB 2.0) host controller as an ADDITIONAL USB host — the
     * UHCI controller above (with its tablet / mass-storage / keyboard) is
     * untouched. No-op if no EHCI controller is attached. ehci_init() resets the
     * HC, builds the async (QH+qTD) schedule, routes the root ports to EHCI,
     * resets the first populated high-speed port, and ENUMERATES the device behind
     * it over control transfers; the self-test logs the HC version + port count,
     * the port reset, and the enumerated device descriptor (idVendor/idProduct/
     * class) read over EHCI — the headless proof, like the storage self-tests. */
    ehci_init();
    ehci_selftest();

    /* Bring up an xHCI (USB 3.0) host controller as an ADDITIONAL USB host — the
     * UHCI + EHCI controllers above (with their tablet / mass-storage / keyboard)
     * are untouched. No-op if no xHCI controller is attached. xhci_init() resets
     * the HC, sets up the device-context base-address array + command ring + event
     * ring, runs the controller, resets the first populated root port, ENABLE SLOT
     * + ADDRESS DEVICE for the device behind it, and ENUMERATES it over EP0 control
     * transfers (TRB rings); the self-test logs the HC version + slot/port counts,
     * the ENABLE SLOT slot id, the port reset, and the enumerated device descriptor
     * (idVendor/idProduct/class) read over xHCI — the headless proof, like the
     * EHCI/storage self-tests. Completes the USB host-controller trilogy. */
    xhci_init();
    xhci_selftest();

    kprintf("[main] launching the desktop environment...\n");
    speaker_chime();              /* a little startup arpeggio */
    desktop_run();
}
