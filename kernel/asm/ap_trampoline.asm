; ap_trampoline.asm — application-processor (AP) bring-up trampoline.
;
; Assembled as a FLAT binary (nasm -f bin) and copied verbatim to physical
; 0x8000 by the BSP (kernel/smp.c). After the BSP sends a STARTUP IPI with
; vector 0x08, each AP begins executing HERE in 16-bit real mode at CS:IP =
; 0x0800:0x0000 (== physical 0x8000). This code walks the AP up the same ladder
; the BSP climbed in boot/boot.asm — real -> protected (PE) -> PAE -> long (LME)
; -> paging (PG) — reusing the kernel's page tables, then jumps to ap_main().
;
; The kernel PML4 already identity-maps the low 1 GiB, so this page (and the
; parameter block below) stay mapped at the instant paging turns on — that is
; what keeps the very next instruction fetch from triple-faulting.
;
; PARAMETER BLOCK at physical 0x7000 (the BSP fills it before the SIPI):
;   [0x7000] qword  kernel PML4 physical address      -> CR3
;   [0x7008] qword  top of this AP's stack            -> RSP
;   [0x7010] qword  &ap_main (64-bit C entry)          -> call target
; (The AP reports liveness by incrementing a counter inside ap_main, so no
;  alive flag is needed in the block itself.)

[bits 16]
[org 0x8000]                    ; runs at physical 0x8000

AP_PARAM    equ 0x7000
P_PML4      equ AP_PARAM + 0x00
P_STACK     equ AP_PARAM + 0x08
P_ENTRY     equ AP_PARAM + 0x10

ap_entry:
    ; The AP starts with CS=0x0800 (base 0x8000). Far-jump to CS=0 so that
    ; absolute [label] memory references (lgdt, the param block) resolve to the
    ; physical addresses NASM baked in under `org 0x8000`.
    jmp 0x0000:ap_real

ap_real:
    cli
    cld
    xor ax, ax
    mov ds, ax                  ; DS=0 so [ap_gdtr] etc. address physical memory
    mov es, ax
    mov ss, ax
    lgdt [ap_gdtr]              ; load our null/code32/data/code64 GDT

    mov eax, cr0
    or  eax, 1                  ; CR0.PE -> protected mode
    mov cr0, eax
    jmp 0x08:ap_prot32          ; far jump into the 32-bit code segment

[bits 32]
ap_prot32:
    mov ax, 0x10                ; flat data selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov eax, cr4
    or  eax, 1 << 5             ; CR4.PAE
    mov cr4, eax

    mov eax, [P_PML4]           ; kernel PML4 phys (a low frame -> fits 32 bits)
    mov cr3, eax

    ; Probe NX before programming EFER, exactly as boot/boot.asm does: setting
    ; EFER.NXE on a CPU without NX would #GP (triple-fault). Match the BSP so the
    ; kernel's W^X (PTE_NX) mappings stay legal on this core.
    mov eax, 0x80000001
    cpuid
    and edx, 1 << 20            ; isolate the NX feature bit
    mov ebx, edx               ; ebx survives rdmsr/wrmsr

    mov ecx, 0xC0000080         ; EFER MSR
    rdmsr
    or  eax, 1 << 8             ; EFER.LME (long mode enable)
    test ebx, ebx
    jz  .skip_nxe
    or  eax, 1 << 11            ; EFER.NXE
.skip_nxe:
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31            ; CR0.PG -> paging on (now in long-mode compat)
    mov cr0, eax
    jmp 0x18:ap_long64          ; far jump into the 64-bit code segment

[bits 64]
ap_long64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov edi, P_STACK
    mov rsp, [rdi]             ; per-AP stack top (HHDM virtual; mapped by PML4)
    mov edi, P_ENTRY
    mov rax, [rdi]
    call rax                   ; ap_main() — does not return
.hang:
    cli
    hlt
    jmp .hang

; --- GDT: null / 32-bit code / flat data / 64-bit code ----------------------
align 8
ap_gdt:
    dq 0                                          ; 0x00 null
    dq 0x00CF9A000000FFFF                          ; 0x08 code32 (4 GiB, exec/read)
    dq 0x00CF92000000FFFF                          ; 0x10 data  (4 GiB, read/write)
    dq (1<<43)|(1<<44)|(1<<47)|(1<<53)             ; 0x18 code64 (exec, present, long)
ap_gdt_end:
ap_gdtr:
    dw ap_gdt_end - ap_gdt - 1                     ; limit
    dd ap_gdt                                      ; base (absolute, via org 0x8000)
