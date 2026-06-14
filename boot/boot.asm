; boot.asm — Multiboot1 entry + 32-bit -> 64-bit long mode trampoline
;
; The bootloader (QEMU's built-in multiboot loader, via `-kernel`) finds the
; Multiboot header below, loads our ELF, and jumps to `_start` in 32-bit
; protected mode with paging OFF. Our job here, in 32-bit code:
;
;   1. Sanity-check that we were actually booted by a multiboot loader.
;   2. Check the CPU actually supports 64-bit long mode (via CPUID).
;   3. Build a minimal set of page tables that identity-maps the low 1 GiB.
;   4. Flip the CPU into long mode (PAE + EFER.LME + CR0.PG).
;   5. Load a 64-bit GDT and far-jump into 64-bit code, which calls kmain().
;
; Everything here is the classic osdev "x86_64 bare bones" trampoline.

MB_MAGIC    equ 0x1BADB002          ; multiboot1 magic the loader searches for
MB_FLAGS    equ (1 << 0) | (1 << 1) ; bit0: page-align modules, bit1: give us a memory map
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

; ---------------------------------------------------------------------------
; Multiboot header — must be 4-byte aligned and within the first 8 KiB of the
; final binary. The linker script places this section first to guarantee that.
; ---------------------------------------------------------------------------
section .multiboot
align 4
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM

; ---------------------------------------------------------------------------
; 32-bit entry point
; ---------------------------------------------------------------------------
section .text
bits 32
global _start
_start:
    mov esp, stack_top              ; we have a stack now

    ; eax/ebx are set by the bootloader; stash the multiboot info pointer (ebx)
    ; so a future kmain can read the memory map. (eax = magic, ebx = info ptr)
    mov [multiboot_info_ptr], ebx

    call check_multiboot
    call check_cpuid
    call check_long_mode

    call setup_page_tables
    call enable_paging

    lgdt [gdt64.pointer]            ; load the 64-bit GDT
    jmp gdt64.code:long_mode_start  ; far jump reloads CS -> we are now in 64-bit

    ; should never get here
    hlt

; --- checks ----------------------------------------------------------------

; Did a multiboot-compliant loader boot us? It leaves 0x2BADB002 in eax.
check_multiboot:
    cmp eax, 0x2BADB002
    jne .no_multiboot
    ret
.no_multiboot:
    mov al, "0"
    jmp error

; Is CPUID available? Try to flip the ID bit (bit 21) in EFLAGS.
check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx                        ; restore original EFLAGS
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret
.no_cpuid:
    mov al, "1"
    jmp error

; Does the CPU support long mode? Check the extended CPUID leaf.
check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode                ; extended functions not available
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29               ; LM bit
    jz .no_long_mode
    ret
.no_long_mode:
    mov al, "2"
    jmp error

; --- paging ----------------------------------------------------------------

; Identity-map the first 1 GiB using 2 MiB pages:
;   PML4[0] -> PDPT[0] -> PD[0..511] each a 2 MiB page.
; We always write the high dword of each entry too, so we never depend on the
; loader having zeroed our .bss.
setup_page_tables:
    ; PML4[0] = PDPT | present | writable
    mov eax, pdpt_table
    or eax, 0b11
    mov [pml4_table], eax
    mov dword [pml4_table + 4], 0

    ; PDPT[0] = PD | present | writable
    mov eax, pd_table
    or eax, 0b11
    mov [pdpt_table], eax
    mov dword [pdpt_table + 4], 0

    ; PD[ecx] = (ecx * 2 MiB) | present | writable | huge
    xor ecx, ecx
.map_pd:
    mov eax, ecx
    shl eax, 21                     ; ecx * 2 MiB
    or eax, 0b10000011              ; present | writable | huge
    mov [pd_table + ecx*8], eax
    mov dword [pd_table + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .map_pd
    ret

enable_paging:
    mov eax, pml4_table             ; CR3 = address of PML4
    mov cr3, eax

    mov eax, cr4                    ; enable PAE (CR4.PAE)
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080             ; EFER MSR
    rdmsr
    or eax, 1 << 8                  ; set LME (long mode enable)
    wrmsr

    mov eax, cr0                    ; enable paging (CR0.PG)
    or eax, 1 << 31
    mov cr0, eax
    ret

; --- error: print "ERR: X" in red to VGA text buffer, then halt ------------
error:
    mov dword [0xb8000], 0x4f524f45 ; "ER"
    mov dword [0xb8004], 0x4f3a4f52 ; "R:"
    mov dword [0xb8008], 0x4f204f20 ; "  "
    mov byte  [0xb800a], al         ; the error code character
.hang:
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
; 64-bit entry point
; ---------------------------------------------------------------------------
bits 64
long_mode_start:
    ; reload data segment registers with the 64-bit data selector
    mov ax, gdt64.data
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, stack_top              ; re-establish stack as a clean 64-bit rsp

    ; pass the multiboot info pointer to kmain as the first argument (rdi).
    ; it's a low physical address, so a 32-bit load zero-extended into rdi.
    mov edi, [multiboot_info_ptr]

    extern kmain
    call kmain                      ; into C — should not return

.hang:
    cli
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
; Read-only data: the 64-bit GDT
; ---------------------------------------------------------------------------
section .rodata
gdt64:
    dq 0                                                ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)            ; code: exec, type, present, long-mode
.data: equ $ - gdt64
    dq (1<<41) | (1<<44) | (1<<47)                      ; data: writable, type, present
.pointer:
    dw $ - gdt64 - 1                                    ; limit
    dq gdt64                                            ; base

; ---------------------------------------------------------------------------
; BSS: page tables (4 KiB aligned) and the boot stack
; ---------------------------------------------------------------------------
section .bss
alignb 4096
pml4_table: resb 4096
pdpt_table: resb 4096
pd_table:   resb 4096

global multiboot_info_ptr
multiboot_info_ptr: resq 1

alignb 16
; 256 KiB boot stack. kmain() -> desktop_run() (the window manager) runs on this
; stack, and the browser executes page <script> there via the JS interpreter,
; whose recursion can be stack-heavy. There is no guard page, so this matches the
; 256 KiB kernel stack the ring-3 apps get (app.c) for the same JS/TLS code.
; (Identity-mapped: early paging maps the first 1 GiB, kernel+BSS is only ~3 MiB.)
stack_bottom: resb 262144           ; 256 KiB boot stack
stack_top:

; Mark the stack non-executable (silences a linker warning; harmless here).
section .note.GNU-stack noalloc noexec nowrite progbits
