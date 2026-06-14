; context_switch.asm — the heart of multitasking.
;
;   void context_switch(uint64_t *old_rsp, uint64_t new_rsp)
;                                rdi               rsi
;
; Save the current thread's execution context, then restore another's. The only
; state we explicitly save is the **callee-saved** registers (the SysV ABI says
; the C caller already preserved everything else around this call) plus the
; stack pointer itself. Everything a thread is "doing" lives on its stack, so
; swapping rsp swaps threads.
;
; We save rsp into *old_rsp, load new_rsp, and `ret`. For a thread that was
; previously switched out, `ret` resumes it right after its own
; context_switch call. For a brand-new thread, task_create pre-built a fake
; frame so this `ret` lands in a trampoline instead.

section .text
global context_switch
context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp          ; *old_rsp = current stack pointer
    mov rsp, rsi            ; switch to the new thread's stack

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret                     ; resume the new thread

section .note.GNU-stack noalloc noexec nowrite progbits
