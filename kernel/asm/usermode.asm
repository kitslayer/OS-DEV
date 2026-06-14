; usermode.asm — cross the ring 0 <-> ring 3 boundary.
;
; enter_user(entry, user_stack): drop into ring 3. There's no "jump to ring 3"
; instruction — you *return* into it. We fake an interrupt-return frame
; (SS, RSP, RFLAGS, CS, RIP) with user selectors and execute `iretq`, which
; pops it and lands in ring 3 with the user's stack and instruction pointer.
;
; Before doing that we stash the kernel's callee-saved state + stack pointer so
; that SYS_exit can come back here (return_to_kernel) and resume the kernel
; right after the enter_user() call — a one-way longjmp out of userspace.

section .bss
global kernel_resume_rsp
kernel_resume_rsp: resq 1
global user_exit_code
user_exit_code: resq 1

section .text

; void enter_user(uint64_t entry /* rdi */, uint64_t user_stack /* rsi */)
global enter_user
enter_user:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [kernel_resume_rsp], rsp     ; remember how to get back

    mov ax, 0x23                     ; USER_DS (ring 3 data) into the data segs
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    pushfq                           ; take current RFLAGS...
    pop rax
    or rax, 0x200                    ; ...and make sure IF is set in user mode

    push 0x23                        ; SS  = USER_DS
    push rsi                         ; RSP = user stack top
    push rax                         ; RFLAGS
    push 0x1B                        ; CS  = USER_CS
    push rdi                         ; RIP = entry point
    iretq                            ; -> ring 3

; void return_to_kernel(long code /* rdi */)  — does not return to its caller;
; resumes execution right after enter_user() in the kernel.
global return_to_kernel
return_to_kernel:
    mov [user_exit_code], rdi

    mov ax, 0x10                     ; back to KERNEL_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [kernel_resume_rsp]     ; restore the saved kernel stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret                              ; return out of enter_user()

section .note.GNU-stack noalloc noexec nowrite progbits
