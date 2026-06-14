; user_blob.asm — embed the compiled userspace program ELFs into the kernel.
;
; We have no general program loader from disk yet, so each user program rides
; along inside the kernel binary. The build links user/<prog>.c into
; build/<prog>.elf; here we incbin them and export start symbols the kernel's
; program registry (kernel/app.c) reads.

section .rodata
global shell_elf_start, shell_elf_end
global clock_elf_start, clock_elf_end
global calc_elf_start, calc_elf_end
global snake_elf_start, snake_elf_end
global editor_elf_start, editor_elf_end
global g2048_elf_start, g2048_elf_end
global life_elf_start, life_elf_end
global tetris_elf_start, tetris_elf_end
global breakout_elf_start, breakout_elf_end
global mines_elf_start, mines_elf_end

shell_elf_start:
    incbin "build/shell.elf"
shell_elf_end:

clock_elf_start:
    incbin "build/clock.elf"
clock_elf_end:

calc_elf_start:
    incbin "build/calc.elf"
calc_elf_end:

snake_elf_start:
    incbin "build/snake.elf"
snake_elf_end:

editor_elf_start:
    incbin "build/editor.elf"
editor_elf_end:

g2048_elf_start:
    incbin "build/g2048.elf"
g2048_elf_end:

life_elf_start:
    incbin "build/life.elf"
life_elf_end:

tetris_elf_start:
    incbin "build/tetris.elf"
tetris_elf_end:

breakout_elf_start:
    incbin "build/breakout.elf"
breakout_elf_end:

mines_elf_start:
    incbin "build/mines.elf"
mines_elf_end:

section .note.GNU-stack noalloc noexec nowrite progbits
