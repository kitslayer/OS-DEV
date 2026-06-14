; gdt_flush.asm — load a new GDT and make the CPU actually use it.
;
; lgdt only tells the CPU where the table is; the segment registers still cache
; the *old* descriptors. Data segments (ds/es/ss/...) refresh with a plain
; `mov`, but CS can't be loaded with mov — it only changes via a control-flow
; transfer. In long mode we reload it with a far return: push the new CS and a
; return address, then `retf` pops both, atomically switching CS:RIP.
;
; C calls:  gdt_flush(struct gdtr *ptr)   -> pointer arrives in rdi.

section .text
global gdt_flush
gdt_flush:
    lgdt [rdi]                  ; point the CPU at the new GDT

    mov ax, 0x10                ; KERNEL_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    pop rax                     ; rax = our return address
    push 0x08                   ; KERNEL_CS  (popped into CS by retf)
    push rax                    ; return address (popped into RIP by retf)
    retfq                       ; far return: reloads CS:RIP -> back to caller

section .note.GNU-stack noalloc noexec nowrite progbits
