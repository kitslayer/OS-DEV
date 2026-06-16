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

#define SYSCALL_VECTOR 0x80

#ifdef __KERNEL__
struct registers;
void syscall_dispatch(struct registers *regs);
#endif
