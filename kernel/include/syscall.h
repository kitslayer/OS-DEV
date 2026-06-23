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

#define SYSCALL_VECTOR 0x80

#ifdef __KERNEL__
struct registers;
void syscall_dispatch(struct registers *regs);
#endif
