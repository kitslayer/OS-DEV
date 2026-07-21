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
MB_FLAGS    equ (1 << 0) | (1 << 1) | (1 << 2) ; bit0: page-align, bit1: memory map, bit2: request a linear framebuffer (GRUB / bare metal)
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
    ; address fields (header offsets 12..28): unused — we're an ELF, so flag
    ; bit16 is clear and the loader ignores these; emit zeros so the video
    ; fields below land at their fixed spec offset 32.
    dd 0, 0, 0, 0, 0
    ; video request (offsets 32..44, honored because flag bit2 is set): ask the
    ; loader (GRUB on real hardware, or QEMU) for a LINEAR 32-bpp framebuffer.
    ; It reports the LFB base + geometry back in the multiboot info, which kmain
    ; consumes via fb_init_mb() (falling back to the Bochs std-VGA path if absent).
    dd 0            ; mode_type: 0 = linear graphics framebuffer
    dd 1280         ; preferred width
    dd 960          ; preferred height
    dd 32           ; preferred depth (bpp)

; --- Multiboot2 header (bare-metal graphics) -------------------------------
; GRUB reliably hands a kernel a framebuffer via the Multiboot2 framebuffer
; TAG (which, unlike Multiboot1's video request, it actually fills in). Boot
; this with grub.cfg `multiboot2`; QEMU's -kernel keeps using the Multiboot1
; header above. The 32-bit entry below handles either magic.
align 8
mb2_start:
    dd 0xE85250D6                                 ; Multiboot2 magic
    dd 0                                           ; architecture: 0 = i386
    dd mb2_end - mb2_start                         ; header length
    dd -(0xE85250D6 + 0 + (mb2_end - mb2_start))   ; checksum (sums to 0 with the 3 above)
    align 8
    dw 5                                           ; tag type 5 = framebuffer request
    dw 0                                           ; flags
    dd 20                                          ; tag size
    dd 1280                                        ; preferred width
    dd 960                                         ; preferred height
    dd 32                                          ; preferred depth (bpp)
    align 8
    dw 0                                           ; end tag (type 0)
    dw 0
    dd 8
mb2_end:

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
    mov [multiboot_magic], eax      ; stash the boot magic too (MB1 0x2BADB002 / MB2 0x36d76289)

    ; Real-hardware bring-up heartbeat (M1873): a single PC-speaker beep the instant
    ; we reach the kernel entry, BEFORE any check that could halt. On a headless/dark
    ; box this is the "GRUB handed off and the kernel is executing" signal. Pure port
    ; I/O, no memory/paging needed, so it works at the earliest possible moment.
    call beep32

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
    cmp eax, 0x2BADB002      ; Multiboot1 loader (QEMU -kernel, GRUB `multiboot`)
    je .ok
    cmp eax, 0x36d76289      ; Multiboot2 loader (GRUB `multiboot2`)
    je .ok
    jmp .no_multiboot
.ok:
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
; NOTE (M1873): a 4 GiB map was tried to reach a high GRUB-placed multiboot info,
; but mapping the low 4 GiB as cached WB huge pages also covers the LAPIC/IOAPIC
; MMIO (0xFEE00000/0xFEC00000) as cached -> EOIs never reach the APIC -> interrupt
; storm -> stack overflow (seen under TCG; KVM virtualizes the APIC so it hid it).
; A larger identity map must skip/UC the MMIO hole; kept at the proven 1 GiB until
; a real-hardware signal says the info pointer actually lands above it.
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

    ; Probe NX support (CPUID 0x80000001, EDX bit 20) before programming EFER.
    ; Setting EFER.NXE on a CPU without NX would #GP (triple-fault), so guard it.
    ; The multiboot pointer is already saved to memory, so clobbering regs is OK.
    mov eax, 0x80000001
    cpuid
    and edx, 1 << 20                ; isolate the NX feature bit
    mov ebx, edx                    ; stash it (ebx survives rdmsr/wrmsr)

    mov ecx, 0xC0000080             ; EFER MSR
    rdmsr
    or eax, 1 << 8                  ; set LME (long mode enable)
    test ebx, ebx                   ; NX supported?
    jz .skip_nxe
    or eax, 1 << 11                 ; set NXE -> the no-execute PTE bit (W^X)
.skip_nxe:
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

; --- beep32: one ~440 Hz PC-speaker beep (bring-up heartbeat, M1873) ---------
; PIT channel 2 square wave gated to the speaker (port 0x61 bits 0+1). Pure port
; I/O — no memory, paging, or IDT needed, so it runs at the earliest instant.
; Clobbers eax/ecx (both free at the _start call site).
beep32:
    push eax                        ; PRESERVE eax — check_multiboot (right after the
    push ecx                        ; call site) still needs the boot magic GRUB left in it
    mov al, 0xB6                    ; PIT: ch2, access lo/hi byte, mode 3 (square)
    out 0x43, al
    mov ax, 2711                    ; divisor 1193182/2711 ~= 440 Hz
    out 0x42, al                    ; low byte
    mov al, ah
    out 0x42, al                    ; high byte
    in al, 0x61
    or al, 0b11                     ; speaker enable (bit0) + PIT gate (bit1)
    out 0x61, al
    mov ecx, 0x08000000             ; crude busy-wait "on" time (~fraction of a second)
.on:
    dec ecx
    jnz .on
    in al, 0x61
    and al, 0b11111100             ; speaker + gate off
    out 0x61, al
    pop ecx
    pop eax                         ; restore the boot magic for check_multiboot
    ret

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

    ; pass the multiboot info pointer (rdi) + the boot magic (rsi) to kmain. Both
    ; are low values, so 32-bit loads zero-extend cleanly. kmain branches on the
    ; magic to read either a Multiboot1 struct or a Multiboot2 tag list.
    mov edi, [multiboot_info_ptr]
    mov esi, [multiboot_magic]

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
multiboot_magic:    resq 1

; 256 KiB boot stack. kmain() -> desktop_run() (the window manager) runs on this
; stack, and the browser executes page <script> there via the JS interpreter, whose
; recursion can be stack-heavy. A GUARD PAGE sits just below it: vmm_harden_kernel
; unmaps stack_guard, so an overflow past stack_bottom takes a page fault here
; instead of silently corrupting the page tables / multiboot info below it.
; Page-aligned so stack_guard is exactly the one page below the lowest stack address.
alignb 4096
global stack_guard
stack_guard:  resb 4096             ; guard page — unmapped by W^X (vmm_harden_kernel)
stack_bottom: resb 262144           ; 256 KiB boot stack
stack_top:

; Mark the stack non-executable (silences a linker warning; harmless here).
section .note.GNU-stack noalloc noexec nowrite progbits
