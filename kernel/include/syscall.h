/*
 * syscall.h — the system-call ABI, shared by kernel and userspace.
 *
 * A user program puts the call number in rax and arguments in rdi/rsi/rdx,
 * then executes `int 0x80`. The kernel handler reads those, does the work, and
 * returns a result in rax. (We use the int-0x80 trap, the simplest mechanism;
 * the faster `syscall`/`sysret` instructions are a later optimization.)
 */
#pragma once

#define SYS_write  1   /* (fd, buf, len)  -> bytes written          */
#define SYS_exit   2   /* (code)          -> does not return        */
#define SYS_getpid 3   /* ()              -> process id             */
#define SYS_read   4   /* (fd, buf, len)  -> bytes read (line)      */
#define SYS_list   5   /* (buf, len)      -> formatted dir listing  */
#define SYS_readfile 6 /* (name, buf, len)-> file bytes, or -1      */
#define SYS_time   7   /* (buf, len)      -> "YYYY-MM-DD HH:MM:SS"  */
#define SYS_beep   8   /* (hz, ms)        -> beep the PC speaker     */
#define SYS_sysinfo 9  /* (buf, len)      -> RAM/uptime/task summary */
#define SYS_clear  10  /* ()              -> clear the terminal      */
#define SYS_reboot 11  /* ()              -> reboot the machine      */
#define SYS_writefile 12 /* (name, buf, len) -> bytes written, or -1 */
#define SYS_ping   13  /* ()              -> echo replies from gateway */
#define SYS_resolve 14 /* (host, buf, len)-> 0 + IP string, or -1     */
#define SYS_delete 15  /* (name)          -> 0, or -1                 */
#define SYS_spawn  16  /* (name)          -> launch a program; 0/-1   */
#define SYS_sleep  17  /* (ms)            -> sleep for ms              */
#define SYS_http   18  /* (host,path,buf,max) -> HTTP GET bytes, or -1 */
#define SYS_browse 19  /* (url)           -> open a browser window; 0    */
#define SYS_mkdir  20  /* (path)          -> make a directory; 0/-1      */
#define SYS_chdir  21  /* (path)          -> change current dir; 0/-1    */
#define SYS_tree   22  /* (buf, len)      -> recursive dir listing       */
#define SYS_ps     23  /* (buf, len)      -> running task/process list   */
#define SYS_pollkey 24 /* ()              -> next key, or -1 (non-blocking) */
#define SYS_df     25  /* (buf, len)      -> disk free/total summary       */
#define SYS_find   26  /* (want,buf,len)  -> recursive name search         */
#define SYS_sha256 27  /* (name, hexbuf)  -> SHA-256 of a file as hex; 0/-1 */
#define SYS_crypt  28  /* (name, pass)    -> AES-CTR (de/en)crypt file; 0/-1 */
#define SYS_history 29 /* (buf, len)      -> recent command history; bytes      */
#define SYS_https  30  /* (host,path,buf,max) -> HTTPS GET bytes via TLS 1.3, or -1 */
#define SYS_js     31  /* (src, out, outmax)  -> run JavaScript; output bytes, or -1 */
#define SYS_setcolor 32 /* (idx)            -> set the app text colour (palette 0-15)  */
#define SYS_pinghost 33 /* (host)           -> DNS-resolve + ICMP-echo a host; replies/-1 */
#define SYS_netinfo 34  /* (buf, len)       -> our IP/MAC/gateway/DNS as text; bytes/-1 */
#define SYS_apps   35   /* (buf, len)       -> registered app names, space-separated; bytes */
#define SYS_sha512 36   /* (name, hexbuf)   -> SHA-512 of a file as 128 hex chars; 0/-1 */
#define SYS_screenshot 37 /* (name)         -> save the screen to a BMP file; 0/-1 */
#define SYS_gunzip 38   /* (insrc, outname) -> decompress a .gz file; output bytes, or -1 */
#define SYS_gzip   39   /* (insrc, outname) -> gzip-compress a file; output bytes, or -1 */
#define SYS_unzip  40   /* (zipname)        -> extract a .zip to 8.3 files; files written, or -1 */
#define SYS_untar  41   /* (tarname)        -> extract a .tar/.tar.gz to 8.3 files; files written, or -1 */
#define SYS_sbrk   42   /* (inc)            -> grow the heap by inc bytes; old break, or (void*)-1 */
#define SYS_uptime_ms 43 /* ()              -> milliseconds since boot (monotonic) */
#define SYS_gfx_init 44 /* (w, h)           -> enter graphics mode with a w*h canvas; 0/-1 */
#define SYS_gfx_blit 45 /* (pixels)         -> copy w*h XRGB pixels to the window; 0/-1 */
#define SYS_setkbmode 46 /* (raw)           -> 1 = raw make/break key events, 0 = cooked */
#define SYS_getkbevent 47 /* ()             -> next raw key event, or -1 (non-blocking) */
#define SYS_pcm    48   /* (frames, nframes) -> play 16-bit stereo PCM at 48 kHz (blocks) */
#define SYS_playwav 49  /* (name)          -> play a .wav file (16-bit PCM); 0/-1 */
#define SYS_pcm_stream 50 /* (frames,nframes)-> queue stereo PCM (non-blocking); frames accepted */
#define SYS_pcm_avail 51 /* ()              -> free frames in the streaming ring */
#define SYS_mouse  52   /* ()              -> packed cursor: x|0-15 y|16-31 buttons|32-34 */
#define SYS_playbg 53   /* (name)          -> play a .wav in the background (non-blocking); 0/-1 */
#define SYS_audiostop 54 /* ()             -> stop background audio */
#define SYS_mouse_rel 55 /* ()             -> packed relative motion dx|0-31 dy|32-63 (mouselook) */
#define SYS_caret  56   /* (on)            -> 1 = show system caret (default), 0 = app draws its own */
#define SYS_clip_get 57 /* (buf, max)      -> copy the system clipboard into buf; returns length */
#define SYS_clip_set 58 /* (buf, len)      -> set the system clipboard from buf; 0 */
#define SYS_getarg  59  /* (buf, max)      -> copy this app's launch argument into buf; returns length */
#define SYS_savebmp 60  /* (name, pixels, w, h) -> save a w*h 0x00RRGGBB canvas as a 24-bit BMP; 0/-1 */
#define SYS_setwall 61  /* (name)          -> load image as the desktop wallpaper; 0/-1 */
#define SYS_lspci  62   /* (buf, len)      -> PCI device list as text lines; bytes      */
#define SYS_lsblk  63   /* (buf, len)      -> block devices + FAT32 volumes as text; bytes */
#define SYS_poweroff 64 /* ()              -> enter ACPI S5 (power the machine off); never returns */
#define SYS_kill   65   /* (pid)           -> ask the app with this pid to close; 0 ok / -1 not found */
#define SYS_mounts 66   /* (buf, max)      -> list the read-only disk mounts (/disk1..) as text; bytes */
#define SYS_mmap   67   /* (len)           -> reserve a demand-paged anon region; base VA, or 0 */
#define SYS_munmap 68   /* (addr, len)     -> free an mmap region; 0/-1 */
#define SYS_signal 69   /* (signo, handler, restorer) -> install a ring-3 signal handler; 0 */
#define SYS_raise  70   /* (signo)         -> deliver a signal to self (runs the handler, returns after) */
#define SYS_sigreturn 71/* ()              -> return from a signal handler (restore the saved context) */
#define SYS_getrandom 72/* (buf, len)      -> fill buf with CSPRNG bytes (hardware-seeded); bytes written */
#define SYS_pledge 73   /* (promises)      -> restrict this process to the named syscall classes; 0/-1 */
#define SYS_unveil 74   /* (path, perms)   -> limit filesystem visibility to path (perms "rwc"); 0/-1 */
#define SYS_symlink 75  /* (linkpath, target) -> create a symlink under /tmp; 0/-1 */
#define SYS_jail   76   /* (prog, promises, path) -> spawn prog pre-confined (pledge + optional unveil); 0/-1 */
#define SYS_ringbuf 77  /* (len)           -> a magic mirrored ring buffer (len frames mapped twice); base VA or 0 */
#define SYS_mprotect 78 /* (addr, len, prot) -> change R/W/X of a mapped range (prot: 1=R 2=W 4=X); 0/-1 */
#define SYS_bind   79   /* (from, to)      -> graft path FROM's subtree onto path TO (bind mount); 0/-1 */
#define SYS_dhcp   80   /* ()              -> DHCP DORA: lease IP/gateway/DNS from the server; 0/-1 */
#define SYS_cas_store 81 /* (buf,len,hash32) -> store a blob in the content-addressed store; writes its SHA-256 key; 0/-1 */
#define SYS_cas_fetch 82 /* (hash32,buf,max) -> fetch a blob by its SHA-256 key; bytes/-1 */
#define SYS_tftp   83   /* (server,file,buf,max) -> fetch a file over TFTP into buf; bytes/-1 */
#define SYS_madvise 84  /* (addr,len,advice) -> MADV_DONTNEED(4): reclaim resident anon pages now; pages/-1 */
#define SYS_alarm  85   /* (ticks) -> arm a periodic SIGALRM every `ticks` timer ticks (0 = disarm); 0 */
#define SYS_sntp   86   /* ()      -> SNTP: set the wall clock from pool.ntp.org; 0/-1 */
#define SYS_swapout 87  /* (addr,len) -> page out anon pages in range to swap (MADV_PAGEOUT); pages/-1 */
#define SYS_losetup 88  /* (path) -> mount a FAT/ext2 image file as a loop block device; mount index (/disk<n+1>) or -1 */
#define SYS_shm_open 89  /* (name, size) -> map a named shared-memory object into this app; base VA or 0 */
#define SYS_futex  90   /* (uaddr, op, val) -> FUTEX_WAIT(0): block if *uaddr==val; FUTEX_WAKE(1): wake up to val waiters */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define SYS_fork   91   /* () -> copy-on-write fork: child returns 0, parent returns the child's pid; -1 on failure */
#define SYS_waitpid 92  /* (pid, int *status) -> block until child (pid, or -1=any) exits; returns its pid + *status; -1 if none */
#define SYS_exec   93   /* (name, arg) -> replace this process's program image with the registered program `name`; -1 on failure (no return on success) */
#define SYS_unshare 94  /* () -> detach into a private mount namespace (later binds become private); 0/-1 */
#define SYS_singlestep 95 /* (n) -> hardware single-step the next n user instructions; read the trace via /proc/<pid>/sstrace */
#define SYS_seccomp       96 /* (nr) -> child: trap syscall `nr` to a supervisor (seccomp-notify) */
#define SYS_seccomp_wait  97 /* (childpid, ev[4]) -> supervisor: block until the child parks; ev={nr,a,b,c}; 1/0/-1 */
#define SYS_seccomp_reply 98 /* (childpid, run_real, retval) -> supervisor: allow(run_real=1)/deny/emulate; 0/-1 */
#define SYS_fswait        99 /* (names, n, timeout_ms) -> block until one of n NUL-separated paths is readable; index/-1 timeout */
#define SYS_signalfd     100  /* (mask) -> route the masked signos to /proc/self/sigfd (read returns the signo) instead of a handler */
#define SYS_fanotify_serve  101 /* () -> become the /fan userspace-materialization daemon; 0/-1 */
#define SYS_fanotify_wait   102 /* (namebuf, max) -> block until a /fan read request; fills the name; len/-1 */
#define SYS_fanotify_provide 103 /* (content, len) -> hand bytes to the blocked /fan reader; len/-1 */
#define SYS_io_uring_enter  104 /* (ring) -> drain a userspace submission ring; ops completed, or -1 (M1129) */
#define SYS_mseal           105 /* (addr, len) -> irreversibly seal mmap regions in range (no later munmap/mprotect); count/-1 (M1130) */
#define SYS_tcp_serve       106 /* (port, resp, resp_len, reqbuf, reqmax) -> serve one TCP connection; request bytes/-1 (M1133) */
#define SYS_uffd_register   107 /* (addr, len) -> route this region's page faults to a monitor (userfaultfd); 0/-1 (M1134) */
#define SYS_uffd_read       108 /* () -> monitor: block until the owner faults; the faulting page addr, or -1 */
#define SYS_uffd_copy       109 /* (addr, data, len) -> monitor: fill the faulting page + wake the owner; 0/-1 */
#define SYS_mmap_file       110 /* (path, len) -> demand-paged file-backed mmap (MAP_PRIVATE); base VA, or 0 (M1136) */
#define SYS_clone           111 /* (fn, stack, arg) -> spawn a thread sharing this address space; tid, or -1 (M1138) */
#define SYS_gettid          112 /* () -> the calling thread's id (its task id) (M1138) */
#define SYS_thread_exit     113 /* () -> end just the calling thread (not the process) (M1138) */
#define SYS_join            114 /* (tid) -> block until thread tid exits, then reap it; 0/-1 (M1139) */
#define SYS_set_tls         115 /* (base) -> set this thread's %fs (TLS) base; 0 (M1140) */
#define SYS_set_robust_list 116 /* (robust_t*) -> register this thread's robust-futex list; 0 (M1141) */
#define SYS_overlay         117 /* (lower, upper) -> mount a union overlay at /over (copy-up to upper); 0/-1 (M1142) */
#define SYS_mincore         118 /* (addr, len, vec) -> per-page residency of an mmap range; 0/-1 (M1147) */
#define SYS_mlock           119 /* (addr, len) -> pin mmap pages against reclaim; 0/-1 (M1149) */
#define SYS_munlock         120 /* (addr, len) -> unpin mlock'd mmap pages; 0/-1 (M1149) */
#define SYS_getrusage       121 /* (who, struct rusage*) -> fill resource usage; 0/-1 (M1150) */
#define SYS_fiemap          122 /* (path, struct fiemap_extent*, max) -> file physical extent map; count/-1 (M1152) */
#define SYS_fallocate       123 /* (path, mode, offset, len) -> punch a hole / preallocate; blocks-affected/-1 (M1153) */
#define SYS_mq_open         124 /* (name, maxmsg, msgsize) -> open/create a priority msg queue; index/-1 (M1154) */
#define SYS_mq_send         125 /* (idx, buf, len, prio) -> enqueue (blocks if full); bytes/-1 (M1154) */
#define SYS_mq_receive      126 /* (idx, buf, max, uint*prio) -> dequeue highest prio (blocks if empty); bytes/-1 (M1154) */
#define SYS_mmap_huge       127 /* (len) -> reserve a 2 MiB-backed demand-paged region (MAP_HUGETLB); base VA or 0 (M1155) */
#define SYS_semget          128 /* (key, nsems, flags) -> SysV semaphore-set id; -1 (M1159) */
#define SYS_semop           129 /* (semid, struct sembuf*, nsops) -> atomic all-or-nothing semop; 0/-1 (M1159) */
#define SYS_semctl          130 /* (semid, semnum, cmd, arg) -> SETVAL/GETVAL/IPC_RMID; value/0/-1 (M1159) */
#define SYS_msgget          131 /* (key, flags) -> SysV message-queue id; -1 (M1160) */
#define SYS_msgsnd          132 /* (msqid, msgbuf*, msgsz, flags) -> enqueue a typed message; 0/-1 (M1160) */
#define SYS_msgrcv          133 /* (msqid, msgbuf*, msgsz, mtyp) -> receive by type; bytes/-1 (M1160) */
#define SYS_shmget          134 /* (key, size, flags) -> SysV shared-memory segment id; -1 (M1161) */
#define SYS_shmat           135 /* (shmid) -> attach: base VA, or 0 (M1161) */
#define SYS_shmdt           136 /* (addr) -> detach a SysV shm mapping; 0/-1 (M1161) */
#define SYS_process_vm_read 137 /* (pid, raddr, local, len) -> read another process's memory; bytes/-1 (M1162) */
#define SYS_getrlimit       138 /* (resource, struct rlimit*) -> read a resource limit; 0/-1 (M1163) */
#define SYS_setrlimit       139 /* (resource, struct rlimit*) -> set a resource limit; 0/-1 (M1163) */
#define SYS_process_vm_write 140 /* (pid, raddr, local, len) -> write another process's memory (COW-safe); bytes/-1 (M1165) */
#define SYS_unix_listen  141   /* (path) -> AF_UNIX listener id; -1 (M1169) */
#define SYS_unix_connect 142   /* (path) -> client endpoint id; -1 (M1169) */
#define SYS_unix_accept  143   /* (lid) -> server endpoint id (blocks); -1 (M1169) */
#define SYS_unix_send    144   /* (ep, buf, len) -> bytes written; -1 closed (M1169) */
#define SYS_unix_recv    145   /* (ep, buf, max) -> bytes read; 0 EOF; -1 bad ep (M1169) */
#define SYS_unix_close   146   /* (ep) -> 0/-1 (M1169) */
#define SYS_unix_wait_any 147  /* (int*eps, n) -> index of first readable ep (blocks once); -1 (M1170) */

/* getrlimit/setrlimit (M1163), shared by the kernel + ulib. */
#define RLIMIT_DATA    2
#define RLIMIT_NPROC   6
#define RLIMIT_AS      9
#define RLIM_INFINITY  (~0UL)
struct rlimit { unsigned long rlim_cur, rlim_max; };

/* System V semaphore ABI (M1159), shared by the kernel + ulib. */
#define IPC_PRIVATE 0
#define IPC_CREAT   0x200
#define IPC_NOWAIT  0x800
#define IPC_RMID    0
#define GETVAL      12
#define SETVAL      16
struct sembuf { short sem_num; short sem_op; short sem_flg; };

/* fallocate(2) modes (M1153). PUNCH_HOLE deallocates whole blocks in the range,
 * leaving a sparse hole that reads as zeros; it must be OR'd with KEEP_SIZE. */
#define FALLOC_FL_KEEP_SIZE  0x01
#define FALLOC_FL_PUNCH_HOLE 0x02

#define SYSCALL_VECTOR 0x80

/* FIEMAP (M1152): a file's physical on-disk extent list (ext2 mounts). Each
 * extent is a maximal run of contiguous physical blocks. Shared kernel/user. */
#define FIEMAP_EXTENT_LAST 0x1            /* this is the file's last extent */
struct fiemap_extent {
    unsigned long fe_logical;            /* byte offset within the file   */
    unsigned long fe_physical;           /* byte offset on the device     */
    unsigned long fe_length;             /* length of the extent in bytes */
    unsigned int  fe_flags;              /* FIEMAP_EXTENT_* */
    unsigned int  _pad;
};

/* getrusage(2) (M1150). RUSAGE_SELF reports the calling process; RUSAGE_CHILDREN
 * is accepted but reports zeros (no child-time accumulation). Shared by the
 * kernel filler and the ulib wrapper, so the layout cannot drift. */
#define RUSAGE_SELF      0
#define RUSAGE_CHILDREN (-1)
struct ru_timeval { long tv_sec; long tv_usec; };
struct rusage {
    struct ru_timeval ru_utime;   /* user-mode CPU time   */
    struct ru_timeval ru_stime;   /* kernel-mode CPU time */
    long ru_maxrss;               /* resident set size, KiB */
    long ru_minflt;               /* minor page faults (no I/O: demand-zero + COW) */
    long ru_majflt;               /* major page faults (disk I/O: swap-in + file-backed) */
    long ru_nvcsw;                /* voluntary context switches (blocked/yielded)   */
    long ru_nivcsw;               /* involuntary context switches (preempted)       */
};

#ifdef __KERNEL__
struct registers;
void syscall_dispatch(struct registers *regs);
#endif
