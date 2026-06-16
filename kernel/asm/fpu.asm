; fpu.asm — enable the x87 FPU + SSE so userspace can use floating point.
;
; The kernel itself is built -mgeneral-regs-only (it never touches the FPU/SSE
; registers), but a userspace program compiled with floating point — notably
; DOOM — needs SSE available. We enable it globally here at boot:
;
;   CR0.EM (bit 2) = 0  use the real FPU, don't trap FP ops to an emulator
;   CR0.MP (bit 1) = 1  monitor coprocessor
;   CR4.OSFXSR     = 1  enable SSE + the FXSAVE/FXRSTOR area layout
;   CR4.OSXMMEXCPT = 1  deliver unmasked SSE exceptions as #XM (not #UD)
;   fninit + MXCSR = 0x1F80  reset x87 and mask all SSE exceptions
;
; We do NOT save/restore the FP/SSE state on a context switch: the kernel and
; every other userspace program are -mgeneral-regs-only, so XMM/x87 is touched
; by at most one task (the FP-using app), and its register contents therefore
; survive being preempted untouched. (If a second FP-using program is ever added,
; add FXSAVE/FXRSTOR to context_switch then.)

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
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
