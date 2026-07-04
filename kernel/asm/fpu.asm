; fpu.asm — enable the x87 FPU + SSE so userspace can use floating point, and
; provide FXSAVE/FXRSTOR so the scheduler can preserve FP/SSE state per task.
;
; The kernel itself is built -mgeneral-regs-only (it never touches the FPU/SSE
; registers), but userspace programs compiled with floating point — DOOM and
; Quake — need SSE. fpu_init enables it globally at boot:
;
;   CR0.EM (bit 2) = 0  use the real FPU, don't trap FP ops to an emulator
;   CR0.MP (bit 1) = 1  monitor coprocessor
;   CR4.OSFXSR     = 1  enable SSE + the FXSAVE/FXRSTOR area layout
;   CR4.OSXMMEXCPT = 1  deliver unmasked SSE exceptions as #XM (not #UD)
;   fninit + MXCSR = 0x1F80  reset x87 and mask all SSE exceptions
;
; It then snapshots this clean state into fpu_template, which task.c copies into
; each new task's save area so a first FXRSTOR loads a sane state. The scheduler
; calls fpu_save/fpu_restore (FXSAVE/FXRSTOR) around every context switch, so two
; FP-using programs (e.g. DOOM and Quake) can run at once without corrupting each
; other's XMM/x87 registers.

section .text
global fpu_init
fpu_init:
    mov rax, cr0
    btr rax, 2          ; clear CR0.EM
    bts rax, 1          ; set   CR0.MP
    mov cr0, rax
    mov rax, cr4
    bts rax, 9          ; set CR4.OSFXSR
    bts rax, 10         ; set CR4.OSXMMEXCPT
    mov cr4, rax
    fninit              ; reset the x87 FPU
    push 0x1F80         ; default MXCSR: all SSE exceptions masked
    ldmxcsr [rsp]
    add rsp, 8
    fxsave [fpu_template]   ; capture the clean state as the per-task template
    ret

; void fpu_init_ap(void) — CR0/CR4 are PER-CORE control registers (M1531): the
; BSP's fpu_init() above only ever enabled FXSAVE/FXRSTOR on the BSP itself.
; Once a task (with its own fxbuf) can be scheduled onto an AP (M1531's cross-
; core scheduler), that AP's FIRST fpu_save/fpu_restore call would execute
; FXSAVE/FXRSTOR with CR4.OSFXSR still 0 -- #UD (Invalid Opcode), hit exactly
; that as a real in-guest crash. Same CR0/CR4 bits, but skips capturing
; fpu_template again: it's a static "clean FPU state" snapshot, identical on
; every core, already captured once by the BSP -- every task's fxbuf is
; seeded from that same shared copy (task.c's fx_alloc), so there is nothing
; core-specific left to (re-)do here.
global fpu_init_ap
fpu_init_ap:
    mov rax, cr0
    btr rax, 2
    bts rax, 1
    mov cr0, rax
    mov rax, cr4
    bts rax, 9
    bts rax, 10
    mov cr4, rax
    fninit
    push 0x1F80
    ldmxcsr [rsp]
    add rsp, 8
    ret

; void fpu_save(void *area16)    — area must be 16-byte aligned
global fpu_save
fpu_save:
    fxsave [rdi]
    ret

; void fpu_restore(const void *area16)
global fpu_restore
fpu_restore:
    fxrstor [rdi]
    ret

section .bss
align 16
global fpu_template
fpu_template:
    resb 512

section .note.GNU-stack noalloc noexec nowrite progbits
