/*
 * setjmp_impl.c — x86-64 setjmp/longjmp for the Quake port's libc shim.
 *
 * Quake's _Host_Frame arms setjmp(host_abortserver) once per frame; error paths
 * (Host_Error/Host_EndGame and Sys-level aborts) longjmp back to it to abort the
 * current frame.  We implement the real thing in assembly (System V AMD64 ABI):
 * save the callee-saved registers, the stack pointer, and the return address.
 *
 * jmp_buf is unsigned long[8] (see include/setjmp.h):
 *   [0]=rbx [1]=rbp [2]=r12 [3]=r13 [4]=r14 [5]=r15 [6]=rsp [7]=rip
 *
 * Emitted via file-scope asm so the Makefile's plain `user/quake/*.c` rule picks
 * it up (no separate .S build step needed).  longjmp returns `val` from setjmp
 * (and 1 if val==0, per the C standard).
 */

__asm__(
    ".text\n"
    ".globl setjmp\n"
    ".type setjmp,@function\n"
"setjmp:\n"
    "    movq %rbx, 0(%rdi)\n"
    "    movq %rbp, 8(%rdi)\n"
    "    movq %r12, 16(%rdi)\n"
    "    movq %r13, 24(%rdi)\n"
    "    movq %r14, 32(%rdi)\n"
    "    movq %r15, 40(%rdi)\n"
    "    leaq 8(%rsp), %rax\n"      /* rsp as it will be after ret (caller's) */
    "    movq %rax, 48(%rdi)\n"
    "    movq (%rsp), %rax\n"       /* return address */
    "    movq %rax, 56(%rdi)\n"
    "    xorl %eax, %eax\n"         /* setjmp returns 0 on the direct path */
    "    ret\n"
    ".size setjmp,.-setjmp\n"

    ".globl longjmp\n"
    ".type longjmp,@function\n"
"longjmp:\n"
    "    movq 0(%rdi), %rbx\n"
    "    movq 8(%rdi), %rbp\n"
    "    movq 16(%rdi), %r12\n"
    "    movq 24(%rdi), %r13\n"
    "    movq 32(%rdi), %r14\n"
    "    movq 40(%rdi), %r15\n"
    "    movq 48(%rdi), %rsp\n"     /* restore caller-of-setjmp stack pointer */
    "    movl %esi, %eax\n"         /* return value = val */
    "    testl %eax, %eax\n"
    "    jnz 1f\n"
    "    movl $1, %eax\n"           /* longjmp(env,0) makes setjmp return 1 */
    "1:\n"
    "    jmp *56(%rdi)\n"           /* resume at the saved return address */
    ".size longjmp,.-longjmp\n"
);
