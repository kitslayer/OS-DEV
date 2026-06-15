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
global sudoku_elf_start, sudoku_elf_end
global calendar_elf_start, calendar_elf_end
global mandel_elf_start, mandel_elf_end
global piano_elf_start, piano_elf_end
global maze_elf_start, maze_elf_end
global adv_elf_start, adv_elf_end
global matrix_elf_start, matrix_elf_end
global paint_elf_start, paint_elf_end
global hangman_elf_start, hangman_elf_end
global jukebox_elf_start, jukebox_elf_end
global ttt_elf_start, ttt_elf_end

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

sudoku_elf_start:
    incbin "build/sudoku.elf"
sudoku_elf_end:

calendar_elf_start:
    incbin "build/calendar.elf"
calendar_elf_end:

mandel_elf_start:
    incbin "build/mandel.elf"
mandel_elf_end:

piano_elf_start:
    incbin "build/piano.elf"
piano_elf_end:

maze_elf_start:
    incbin "build/maze.elf"
maze_elf_end:

adv_elf_start:
    incbin "build/adv.elf"
adv_elf_end:

matrix_elf_start:
    incbin "build/matrix.elf"
matrix_elf_end:

paint_elf_start:
    incbin "build/paint.elf"
paint_elf_end:

hangman_elf_start:
    incbin "build/hangman.elf"
hangman_elf_end:

jukebox_elf_start:
    incbin "build/jukebox.elf"
jukebox_elf_end:

ttt_elf_start:
    incbin "build/ttt.elf"
ttt_elf_end:

section .note.GNU-stack noalloc noexec nowrite progbits
