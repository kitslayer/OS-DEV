/* ulib.h — tiny userspace C library (the start of a libc). */
#pragma once
#include "robust.h"   /* robust_t + FUTEX_OWNER_DIED, for robust mutexes (M1141) */
#include "syscall.h"  /* SYS_* numbers + shared ABI structs (e.g. struct rusage, M1150) */

/* raw syscalls */
long sys_write(int fd, const void *buf, unsigned long len);
long sys_read(int fd, void *buf, unsigned long len);
void sys_exit(int code);
int  sys_getpid(void);
long sys_fork(void);                 /* COW fork: child returns 0, parent returns child pid, -1 on failure (M1116) */
long sys_waitpid(int pid, int *status);  /* block until child (pid, or -1=any) exits; returns its pid, *status=code (M1117) */
long sys_exec(const char *name, const char *arg);  /* replace this process's image with registered program `name`; -1 fail (M1121) */
long sys_unshare(void);              /* detach into a private mount namespace (later binds are private); 0/-1 (M1122) */
long sys_singlestep(int n);          /* hardware single-step the next n user instructions; read /proc/self/sstrace (M1123) */
/* seccomp-notify: userspace syscall supervision (M1124) */
long sys_seccomp(int nr);            /* child: trap syscall `nr` to a supervisor */
long sys_seccomp_wait(int childpid, unsigned long *ev4);  /* supervisor: block until child parks; ev={nr,a,b,c}; 1/0/-1 */
long sys_seccomp_reply(int childpid, int run_real, long retval);  /* supervisor: allow(run_real=1)/deny/emulate */
long sys_fswait(const char *const *paths, int n, long timeout_ms);  /* block until one of n paths is readable; index/-1 (M1125) */
long sys_poll(struct pollfd *fds, int nfds, long timeout_ms);       /* fd-table readiness multiplex; #ready/0 timeout/-1 (M1210) */
long sys_splice(int in_fd, int out_fd, unsigned long len);         /* move bytes pipe->pipe in-kernel (consumes src); bytes/0/-1 (M1211) */
long sys_tee(int in_fd, int out_fd, unsigned long len);            /* copy bytes pipe->pipe (src preserved); bytes/0/-1 (M1211) */
int  sys_memfd_create(const char *name, int flags);                /* anonymous sealable in-RAM file fd (>=3); -1 (M1212) */
long sys_memfd_seal(int fd, unsigned seals);                       /* add F_SEAL_*; new seal set/-1 (M1212) */
long sys_ftruncate(int fd, long len);                              /* resize a memfd (seal-checked); 0/-1 (M1212) */
long sys_signalfd(unsigned mask);    /* route the masked signos to /proc/self/sigfd instead of a handler (M1126) */
/* fanotify-style userspace file materialization (M1128) */
long sys_fanotify_serve(void);                       /* become the /fan daemon */
long sys_fanotify_wait(char *namebuf, int max);      /* block until a /fan read; fills the name; len/-1 */
long sys_fanotify_provide(const void *content, unsigned long len);  /* hand bytes back to the reader */
long sys_io_uring_enter(void *ring);                 /* drain a struct io_ring of batched ops; # completed/-1 (M1129) */
long sys_mseal(void *addr, unsigned long len);       /* irreversibly seal mmap regions in range vs munmap/mprotect; count/-1 (M1130) */
long sys_tcp_serve(int port, const void *resp, unsigned long resp_len, void *reqbuf, unsigned long reqmax);  /* serve one TCP conn; request bytes/-1 (M1133) */
/* userfaultfd (M1134): register a region, then a monitor process services its faults */
long sys_uffd_register(void *addr, unsigned long len);              /* route this region's faults to a monitor; 0/-1 */
long sys_uffd_read(void);                                           /* monitor: block until a fault; faulting page addr/-1 */
long sys_uffd_copy(void *addr, const void *data, unsigned long len);/* monitor: fill the faulting page + wake the owner; 0/-1 */
long sys_list(void *buf, unsigned long len);
long sys_readfile(const char *name, void *buf, unsigned long len);
long sys_writefile(const char *name, const void *buf, unsigned long len);
long sys_delete(const char *name);
long sys_time(void *buf, unsigned long len);
void sys_beep(int hz, int ms);
long sys_sysinfo(void *buf, unsigned long len);
void sys_clear(void);
void sys_reboot(void);
void sys_poweroff(void);   /* enter ACPI S5: power the machine off; never returns */
long sys_kill(int pid);    /* ask the app with this pid to close; 0 ok / -1 not found */
long sys_ping(void);
long sys_ping_host(const char *host);
long sys_netinfo(void *buf, unsigned long len);
long sys_dhcp(void);
long sys_cas_store(const void *buf, unsigned long len, void *hash32);
long sys_cas_fetch(const void *hash32, void *buf, unsigned long max);
long sys_tftp(const char *filename, void *buf, unsigned long max);
#define MADV_DONTNEED 4    /* drop resident pages now (re-fault zero) */
#define MADV_COLD     20   /* deactivate: clear accessed bits (M1158) */
#define MADV_PAGEOUT  21   /* page the range out to swap/zram now (M1158) */
#define MADV_COLLAPSE 25   /* synchronously fold the range into 2 MiB hugepage(s) (M1168) */
long sys_madvise(void *addr, unsigned long len, int advice);
long sys_mincore(void *addr, unsigned long len, unsigned char *vec);   /* per-page residency; vec[i]=1 if resident; 0/-1 (M1147) */
long sys_mlock(void *addr, unsigned long len);     /* pin mmap pages against reclaim; 0/-1 (M1149) */
long sys_munlock(void *addr, unsigned long len);   /* unpin mlock'd mmap pages; 0/-1 (M1149) */
long sys_getrusage(int who, struct rusage *ru);    /* fill resource usage (RUSAGE_SELF=0); 0/-1 (M1150) */
long sys_fiemap(const char *path, struct fiemap_extent *out, int max);  /* file physical extent map; count/-1 (M1152) */
long sys_fallocate(const char *path, int mode, unsigned long offset, unsigned long len);  /* punch hole (FALLOC_FL_PUNCH_HOLE); blocks/-1 (M1153) */
long sys_mq_open(const char *name, int maxmsg, int msgsize);   /* open/create priority msg queue; index/-1 (M1154) */
long sys_mq_send(int idx, const void *buf, unsigned long len, unsigned int prio);    /* enqueue; bytes/-1 (M1154) */
long sys_mq_receive(int idx, void *buf, unsigned long max, unsigned int *prio);      /* dequeue highest prio; bytes/-1 (M1154) */
long sys_semget(int key, int nsems, int flags);                    /* SysV semaphore set; id/-1 (M1159) */
long sys_semop(int semid, struct sembuf *sops, unsigned nsops);    /* atomic all-or-nothing semop; 0/-1 (M1159) */
long sys_semctl(int semid, int semnum, int cmd, int arg);          /* SETVAL/GETVAL/IPC_RMID (M1159) */
long sys_msgget(int key, int flags);                               /* SysV message queue; id/-1 (M1160) */
long sys_msgsnd(int id, const void *msgp, unsigned long sz, int flags);   /* enqueue {long mtype; data} (M1160) */
long sys_msgrcv(int id, void *msgp, unsigned long sz, long mtyp);  /* receive by type; bytes/-1 (M1160) */
long sys_shmget(int key, unsigned long size, int flags);          /* SysV shm segment; id/-1 (M1161) */
void *sys_shmat(int shmid);                                       /* attach: base VA or 0 (M1161) */
long sys_shmdt(void *addr);                                       /* detach a shm mapping (M1161) */
long sys_process_vm_read(int pid, unsigned long raddr, void *buf, unsigned long len);  /* read another process's mem; bytes/-1 (M1162) */
long sys_ptrace(long req, int pid, unsigned long addr, unsigned long data);  /* trace a child process (M1199) */
long sys_bpf_trace(const void *prog, unsigned long bytes);  /* load a global syscall-tracepoint BPF program (M1202) */
unsigned long sys_bpf_map_get(unsigned idx);                /* read a BPF histogram cell (M1202) */
long sys_process_vm_write(int pid, unsigned long raddr, const void *buf, unsigned long len);  /* write another process's mem (COW-safe); bytes/-1 (M1165) */
/* AF_UNIX path-keyed stream sockets (M1169): listen/connect/accept by path, send/recv over an endpoint id (survives fork) */
int  sys_unix_listen(const char *path);                /* -> listener id; -1 */
int  sys_unix_connect(const char *path);               /* -> client endpoint id; -1 */
int  sys_unix_accept(int lid);                         /* -> server endpoint id (blocks); -1 */
long sys_unix_send(int ep, const void *buf, unsigned long len);   /* bytes written; -1 closed */
long sys_unix_recv(int ep, void *buf, unsigned long max);         /* bytes read; 0 EOF; -1 bad ep */
int  sys_unix_close(int ep);                           /* 0/-1 */
int  sys_unix_wait_any(const int *eps, int n);         /* poll: index of first readable ep (blocks once); -1 (M1170) */
int  sys_nice(int nice);                               /* set current task's CFS nice (-20..19); returns clamped (M1171) */
int  sys_sched_setscheduler(int policy, int rt_priority);  /* SCHED_OTHER/FIFO/RR; rt_priority 1..99 for RT; 0/-1 (M1172) */
long sys_statx(const char *path, struct statx *st);        /* file metadata: mode/size/mtime/nlink (M1173) */
int  sys_tcgetattr(struct termios *t);                     /* read the TTY discipline mode (M1174) */
int  sys_tcsetattr(const struct termios *t);               /* set cooked/raw TTY mode (M1174) */
int  sys_setpgid(int pid, int pgid);                       /* job control: set a process group (M1176) */
int  sys_getpgid(int pid);                                 /* process group id (M1176) */
int  sys_setsid(void);                                     /* become session+group leader (M1176) */
int  sys_tcsetpgrp(int pgid);                              /* set the console foreground group (M1176) */
int  sys_killpg(int pgid, int signo);                      /* signal every process in a group (M1176) */
int  sys_flock(const char *path, int op);                  /* advisory whole-file lock LOCK_SH/EX/UN|NB (M1177) */
long sys_getrlimit(int resource, struct rlimit *rl);   /* read a resource limit (M1163) */
long sys_setrlimit(int resource, struct rlimit *rl);   /* set a resource limit (M1163) */
long sys_alarm(unsigned long ticks);
long sys_sntp(void);
long sys_swapout(void *addr, unsigned long len);
long sys_losetup(const char *path);
void *sys_shm_open(const char *name, unsigned long size);
long sys_futex(void *uaddr, int op, int val);
long sys_apps(void *buf, unsigned long len);
long sys_resolve(const char *host, void *buf, unsigned long len);
long sys_http(const char *host, const char *path, void *buf, unsigned long max);
long sys_https(const char *host, const char *path, void *buf, unsigned long max);
long sys_spawn(const char *name);
long sys_spawn_arg(const char *name, const char *arg);   /* launch with an arg (e.g. `run editor FILE`) */
long sys_browse(const char *url);
long sys_mkdir(const char *path);
long sys_chdir(const char *path);
long sys_tree(void *buf, unsigned long len);
long sys_ps(void *buf, unsigned long len);
long sys_history(void *buf, unsigned long len);
int  sys_pollkey(void);
long sys_df(void *buf, unsigned long len);
long sys_lspci(void *buf, unsigned long len);
long sys_lsblk(void *buf, unsigned long len);
long sys_mounts(void *buf, unsigned long len);
long sys_getrandom(void *buf, unsigned long len);   /* fill buf with hardware-seeded CSPRNG bytes */
int  sys_pledge(const char *promises);   /* restrict this process to the named syscall classes; 0/-1 */
int  sys_unveil(const char *path, const char *perms);   /* limit filesystem visibility to path (perms "rwc"); 0/-1 */
int  sys_symlink(const char *linkpath, const char *target);   /* create a symlink under /tmp; 0/-1 */
int  sys_link(const char *oldpath, const char *newpath);       /* hard link (same ext2 mount); 0/-1 (M1207) */
int  sys_rename(const char *oldpath, const char *newpath);     /* rename/move within one ext2 mount; 0/-1 (M1213) */
long sys_prlimit(int pid, int resource, unsigned long newval, int do_set);  /* get/set a process's rlimit; old value (M1214) */
int  sys_timerfd_create(void);                                 /* a pollable one-shot timer fd (>=3); -1 (M1217) */
long sys_timerfd_settime(int fd, long delay_ms);               /* arm a timerfd (ms; <=0 disarms); 0/-1 (M1217) */
long sys_fcntl(int fd, int cmd, long arg);                     /* F_GETFD/SETFD/DUPFD/DUPFD_CLOEXEC (M1218) */
int  sys_dup3(int oldfd, int newfd, int flags);                /* dup w/ O_CLOEXEC; -1 if old==new (M1218) */
long sys_close_range(unsigned lo, unsigned hi, int flags);     /* close fds in [lo,hi]; 0/-1 (M1218) */
long sys_sendfile(int out_fd, int in_fd, long *off, unsigned long count);  /* zero-copy fd->fd; bytes/-1 (M1219) */
int  sys_epoll_create1(int flags);                             /* an epoll fd (>=3); -1 (M1220) */
int  sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev);      /* ADD/MOD/DEL; 0/-1 (M1220) */
int  sys_epoll_wait(int epfd, struct epoll_event *evs, int maxevents, long timeout);  /* # ready/0/-1 (M1220) */
int  sys_pidfd_open(int pid, int flags);                       /* a pollable process-exit handle (>=3); -1 (M1222) */
int  sys_pidfd_send_signal(int pidfd, int sig);                /* signal the pidfd's process; 0/-1 (M1222) */
long sys_getdents64(void *buf, unsigned long max, int start);  /* packed dirent64 of the cwd; bytes/0/-1 (M1223) */
int  sys_access(const char *path, int amode);                  /* 0 if accessible, -1 (M1224) */
int  sys_jail(const char *prog, const char *promises, const char *path);   /* spawn prog pre-confined (pledge + optional unveil) */
long sys_find(const char *want, void *buf, unsigned long len);
long sys_sha256(const char *name, void *hexbuf, unsigned long max);
long sys_sha512(const char *name, void *hexbuf, unsigned long max);
long sys_crypt(const char *name, const char *pass);
long sys_js(const char *src, void *out, unsigned long max);
long sys_screenshot(const char *name);
long sys_savebmp(const char *name, const void *pixels, int w, int h);   /* save a w*h 0x00RRGGBB canvas as a 24-bit BMP; 0/-1 */
long sys_setwall(const char *name);   /* load an image file as the desktop wallpaper; 0/-1 */
long sys_gunzip(const char *insrc, const char *outname);
long sys_gzip(const char *insrc, const char *outname);
long sys_unzip(const char *zipname);
long sys_untar(const char *tarname);
void sys_sleep(int ms);
void sys_setcolor(int color);   /* text colour for subsequent output: palette index 0-15 (0 = default) */
void *sbrk(long inc);           /* grow the heap by inc bytes; previous break, or (void*)-1 */
void *sys_mmap(unsigned long len);              /* reserve a demand-paged anon region; base or 0 */
void *sys_mmap_huge(unsigned long len);         /* reserve a 2 MiB-backed (hugepage) region; base or 0 (M1155) */
void *sys_mmap_file(const char *path, unsigned long len);  /* demand-paged file-backed mmap (MAP_PRIVATE); base or 0 (M1136) */
/* threads (M1138): shared-address-space concurrency (unlike fork's separate space) */
long sys_clone(void *fn, void *stack, void *arg);  /* low-level: start fn(arg) on `stack` in a new thread; tid/-1 */
int  sys_gettid(void);                             /* the calling thread's id */
void sys_thread_exit(void);                        /* end the calling thread (not the process) */
int  thread_spawn(void (*fn)(void *), void *arg);  /* convenience: alloc a stack + clone; tid/-1 */
int  sys_join(int tid);                            /* block until thread tid exits + reap it; 0/-1 (M1139) */
void mutex_lock(volatile int *m);                  /* futex-backed mutex (M1139); lock word: 0=free 1=held */
void mutex_unlock(volatile int *m);
void sys_set_tls(void *base);                      /* set this thread's %fs base for TLS (M1140) */
/* robust mutexes (M1141): survive a thread dying while holding the lock */
long sys_set_robust_list(void *r);                 /* register this thread's robust_t */
int  rmutex_lock(volatile int *m, robust_t *r);    /* 0, or 1 (EOWNERDEAD) if the prior owner died holding it */
void rmutex_unlock(volatile int *m, robust_t *r);
void *sys_ringbuf(unsigned long len);           /* a magic mirrored ring buffer (mapped twice back-to-back); base or 0 */
int   sys_mprotect(void *addr, unsigned long len, int prot);  /* change R/W/X (prot: 1=R 2=W 4=X); 0/-1 */
int   sys_bind(const char *from, const char *to);  /* bind mount: graft FROM's subtree onto path TO; 0/-1 */
long  sys_overlay(const char *lower, const char *upper);  /* mount a union overlay at /over (copy-up to upper); 0/-1 (M1142) */
long  sys_munmap(void *addr, unsigned long len);/* free an mmap region; 0/-1 */
void *sys_mremap(void *old_addr, unsigned long old_len, unsigned long new_len, int flags);  /* resize/move; new base or NULL (M1179) */
long  sys_copy_file_range(const char *src, const char *dst, unsigned long len);  /* in-kernel file copy (dst /net/tcp = sendfile); bytes/-1 (M1181) */
long  sys_setxattr(const char *path, const char *name, const void *val, unsigned long vlen);  /* set a user.* xattr (ext2); vlen/-1 (M1182) */
long  sys_getxattr(const char *path, const char *name, void *out, unsigned long max);  /* read a user.* xattr; full size/-1 (M1182) */
long  sys_listxattr(const char *path, char *out, unsigned long max);  /* NUL-sep user.* xattr names; total/-1 (M1182) */
long  sys_removexattr(const char *path, const char *name);  /* remove a user.* xattr; 0/-1 (M1182) */
int   sys_pty_open(void);                                    /* -> pty master id (slave = master|1); -1 (M1185) */
long  sys_pty_read(int id, void *buf, unsigned long max);    /* bytes; 0 EOF; -1 (M1185) */
long  sys_pty_write(int id, const void *buf, unsigned long len);  /* bytes; -1 (master write feeds the ldisc) (M1185) */
int   sys_pty_close(int id);                                 /* close one end; 0/-1 (M1185) */
int   sys_pty_ctl(int id, int cmd, int arg);                 /* cmd 0=lflag, 1=fg pgid; 0/-1 (M1185) */
int   sys_pipe(int fds[2]);                                  /* anonymous pipe; fds[0]=read, fds[1]=write; 0/-1 (M1187) */
long  sys_fdread(int fd, void *buf, unsigned long max);      /* read a pipe fd; bytes/0 EOF/-1 (M1187) */
long  sys_fdwrite(int fd, const void *buf, unsigned long len);  /* write a pipe fd; bytes/-1 EPIPE (M1187) */
int   sys_fdclose(int fd);                                   /* close an fd; 0/-1 (M1187) */
int   sys_dup2(int oldfd, int newfd);                        /* redirect newfd onto oldfd; newfd/-1 (M1187) */
int   sys_mkfifo(const char *path);                          /* create a named pipe (FIFO); 0/-1 (M1188) */
int   sys_fifo_open(const char *path, int write);            /* open a FIFO end -> fd; -1 (M1188) */
int   sys_open(const char *path);                            /* open a read-only file fd (>=3); -1 (M1193) */
int   sys_open_mode(const char *path, int flags);            /* open a file fd with O_* flags (M1195) */
long  sys_lseek(int fd, long off, int whence);               /* reposition a file fd (SEEK_SET/CUR/END); offset/-1 (M1193) */
long  sys_seccomp_filter(const void *prog, unsigned long bytes);  /* install a self-imposed BPF syscall filter; 0/-1 (M1190) */
#define PTY_SETMODE 0
#define PTY_SETFG   1
long  sys_signal(int signo, void (*handler)(int));  /* install a ring-3 signal handler */
void  sys_raise(int signo);                     /* deliver a signal to self (runs the handler) */
unsigned sys_sigprocmask(int how, unsigned set);  /* block/unblock signals; returns the old mask (M1208) */
unsigned sys_sigpending(void);                    /* the pending (raised-but-blocked) signal set (M1209) */
unsigned long sys_uptime_ms(void);   /* monotonic milliseconds since boot */
int  sys_gfx_init(int w, int h);     /* enter graphics mode: a w*h XRGB pixel canvas; 0/-1 */
int  sys_gfx_blit(const void *pixels); /* copy w*h pixels (0x00RRGGBB) to the window; 0/-1 */
void sys_setkbmode(int raw);         /* 1 = raw make/break key events, 0 = cooked ASCII */
void sys_caret(int on);              /* 1 = show system caret (default), 0 = this app draws its own */
int  sys_clip_get(char *buf, int max);     /* copy the system clipboard into buf; returns length */
void sys_clip_set(const char *buf, int len); /* set the system clipboard (shared with middle-click paste) */
int  sys_getarg(char *buf, int max);       /* copy this app's launch argument into buf; returns length */
int  sys_getkbevent(void);           /* next raw key event (scancode|0x100 released|0x200 ext), or -1 */
void sys_pcm(const void *frames, int nframes);   /* play 16-bit stereo PCM @ 48 kHz (blocks) */
long sys_playwav(const char *name);              /* play a .wav file (16-bit PCM); 0/-1 */
int  sys_pcm_stream(const void *frames, int nframes);  /* queue stereo PCM (non-blocking); accepted */
int  sys_pcm_avail(void);                        /* free frames in the streaming ring */
int  sys_mouse(int *x, int *y);   /* cursor relative to the gfx canvas (-1 outside); returns button bits */
void sys_mouse_rel(int *dx, int *dy);   /* accumulated relative motion since last call (mouselook) */
long sys_playbg(const char *name);   /* play a .wav in the background (non-blocking); 0/-1 */
void sys_audiostop(void);            /* stop background audio */

/* dynamic memory (a first-fit free list over sbrk) */
void *malloc(unsigned long n);
void  free(void *p);
void *calloc(unsigned long nmemb, unsigned long size);
void *realloc(void *p, unsigned long n);

/* freestanding mem primitives (GCC may also emit calls to these) */
void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);
void *memmove(void *dst, const void *src, unsigned long n);

/* convenience */
void          print(const char *s);
void          cap_begin(void);                          /* redirect print() into a growable heap buffer */
char         *cap_end(unsigned long *outlen);           /* stop capturing; returns the malloc'd buffer (caller frees) + byte count */
int           cap_active(void);                         /* nonzero if print() is being captured (pipe stage / $()) — suppress decorative output */
int           readline(char *buf, int max);   /* reads a line, strips '\n', NUL-terminates */
unsigned long ustrlen(const char *s);
int           streq(const char *a, const char *b);
int           startswith(const char *s, const char *prefix);

/* clock_gettime — reads the kernel's vDSO time page directly, with NO syscall
 * (the page is mapped read-only into every process; the timer IRQ refreshes it
 * under a seqlock). M1111. Returns 0. */
#define CLOCK_REALTIME  0   /* wall clock: seconds since the Unix epoch (UTC) */
#define CLOCK_MONOTONIC 1   /* steady time since boot (never jumps) */
struct timespec { long tv_sec; long tv_nsec; };
int clock_gettime(int clk, struct timespec *ts);
