; isr_stubs.asm — per-vector entry points + the shared save/restore path.
;
; The CPU's interrupt frame isn't uniform: some exceptions push a 64-bit error
; code, most don't. To give C a single struct layout, every stub makes the
; stack look identical:
;
;     [ err_code ]   <- real one (ERR vectors) or a dummy 0 (NOERR vectors)
;     [ int_no   ]   <- which vector we are
;
; isr_common then pushes all 15 general-purpose registers and calls
; isr_dispatch(rsp). The field order in `struct registers` mirrors this.

extern isr_dispatch

section .text

; vectors that do NOT push an error code: push a dummy 0 to keep layout uniform
%macro ISR_NOERR 1
isr%1:
    push 0
    push %1
    jmp isr_common
%endmacro

; vectors that DO push an error code: the CPU already pushed it
%macro ISR_ERR 1
isr%1:
    push %1
    jmp isr_common
%endmacro

; --- CPU exceptions 0..31 (error-code ones per the Intel SDM) ---------------
ISR_NOERR 0      ; #DE divide error
ISR_NOERR 1      ; #DB debug
ISR_NOERR 2      ; NMI
ISR_NOERR 3      ; #BP breakpoint
ISR_NOERR 4      ; #OF overflow
ISR_NOERR 5      ; #BR bound range
ISR_NOERR 6      ; #UD invalid opcode
ISR_NOERR 7      ; #NM device not available
ISR_ERR   8      ; #DF double fault
ISR_NOERR 9      ; (reserved)
ISR_ERR   10     ; #TS invalid TSS
ISR_ERR   11     ; #NP segment not present
ISR_ERR   12     ; #SS stack-segment fault
ISR_ERR   13     ; #GP general protection
ISR_ERR   14     ; #PF page fault
ISR_NOERR 15     ; (reserved)
ISR_NOERR 16     ; #MF x87 floating point
ISR_ERR   17     ; #AC alignment check
ISR_NOERR 18     ; #MC machine check
ISR_NOERR 19     ; #XM SIMD floating point
ISR_NOERR 20     ; #VE virtualization
ISR_ERR   21     ; #CP control protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28     ; #HV
ISR_ERR   29     ; #VC
ISR_ERR   30     ; #SX
ISR_NOERR 31     ; (reserved)

; --- hardware IRQs 32..47 (remapped PIC; none push an error code) -----------
%assign v 32
%rep 16
    ISR_NOERR v
%assign v v+1
%endrep

; --- the syscall trap: int 0x80 (vector 128), invokable from ring 3 ---------
global isr128
isr128:
    push 0
    push 128
    jmp isr_common

; --- shared tail: save state, call C, restore, return -----------------------
isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld                     ; SysV ABI: direction flag must be clear in C
    mov rdi, rsp            ; first arg = pointer to struct registers
    call isr_dispatch
    ; fall through into the shared return tail
isr_return_tail:           ; resume ring3 from the struct registers at [rsp]
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16             ; discard int_no + err_code
    iretq                   ; restore rip/cs/rflags/rsp/ss, resume

; void iret_to_user(struct registers *r /* rdi */) — resume ring 3 from a cloned
; trap frame (used by fork's child: r->rax preset to 0). Sets user data segments
; (iretq only restores CS/SS), points RSP at the frame, and runs the shared tail.
; Interrupts are masked until the iretq restores RFLAGS (which has IF set).
global iret_to_user
iret_to_user:
    cli
    mov ax, 0x23            ; USER_DS into the data segments
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov rsp, rdi           ; RSP -> the saved struct registers (r15 at the lowest addr)
    jmp isr_return_tail

; --- table the C side reads to fill the IDT ---------------------------------
section .rodata
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 48
    dq isr %+ v
%assign v v+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
