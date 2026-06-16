/* setjmp.h — Quake libc shim.
 *
 * Quake uses setjmp/longjmp for Host_Error / Sys_Error recovery: _Host_Frame
 * arms `setjmp(host_abortserver)` once per frame and various error paths
 * longjmp back to it to abort the current server frame without unwinding by
 * hand.  We provide a real x86-64 setjmp/longjmp in setjmp.S (saving the
 * callee-saved registers + rsp + return address), which is robust across the
 * deep engine call stack — more so than __builtin_setjmp.
 *
 * jmp_buf layout (see setjmp.S): rbx, rbp, r12, r13, r14, r15, rsp, rip.
 */
#ifndef _OSDEV_SETJMP_H
#define _OSDEV_SETJMP_H

typedef unsigned long jmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

/* Quake only ever uses the plain (non-signal) variants, but some builds spell
 * them with the _sig prefix; alias them for safety. */
#define sigsetjmp(env, save) setjmp(env)
#define siglongjmp(env, val) longjmp(env, val)

#endif /* _OSDEV_SETJMP_H */
