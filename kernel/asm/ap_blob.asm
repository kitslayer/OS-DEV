; ap_blob.asm — embed the flat AP trampoline binary into the kernel image.
;
; ap_trampoline.asm is assembled as a raw flat binary (nasm -f bin, see the
; Makefile) because it begins in 16-bit real mode at a fixed physical address.
; Here we incbin that binary so the kernel always carries it; kernel/smp.c
; copies it to physical 0x8000 before sending the STARTUP IPI to each AP.
; (Same pattern as user_blob.asm embedding the userspace ELFs.)

section .rodata
global ap_tramp_start, ap_tramp_end
ap_tramp_start:
    incbin "build/ap_trampoline.bin"
ap_tramp_end:

section .note.GNU-stack noalloc noexec nowrite progbits
