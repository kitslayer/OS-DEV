; user_blob.asm — embed the compiled userspace program ELFs into the kernel.
;
; The built-in apps ride along inside the kernel binary so they're always
; available without a disk. (The kernel can ALSO load an ELF from the FAT32
; disk at runtime -- `run NAME.ELF`, via app_spawn_from_file in kernel/app.c --
; but these embedded ones need no filesystem.) The build links user/<prog>.c
; into build/<prog>.elf; here we incbin them and export the start symbols the
; kernel's program registry (kernel/app.c) reads.

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
global timer_elf_start, timer_elf_end
global mandel_elf_start, mandel_elf_end
global piano_elf_start, piano_elf_end
global maze_elf_start, maze_elf_end
global adv_elf_start, adv_elf_end
global matrix_elf_start, matrix_elf_end
global paint_elf_start, paint_elf_end
global hangman_elf_start, hangman_elf_end
global jukebox_elf_start, jukebox_elf_end
global ttt_elf_start, ttt_elf_end
global bj_elf_start, bj_elf_end
global typing_elf_start, typing_elf_end
global simon_elf_start, simon_elf_end
global c4_elf_start, c4_elf_end
global wordle_elf_start, wordle_elf_end
global gfxdemo_elf_start, gfxdemo_elf_end
global scene3d_elf_start, scene3d_elf_end
global terrain_elf_start, terrain_elf_end
global demoscene_elf_start, demoscene_elf_end
global doom_elf_start, doom_elf_end
global quake_elf_start, quake_elf_end
global nes_elf_start, nes_elf_end
global reversi_elf_start, reversi_elf_end
global lights_elf_start, lights_elf_end
global fifteen_elf_start, fifteen_elf_end
global mastermind_elf_start, mastermind_elf_end
global pong_elf_start, pong_elf_end
global halflife_elf_start, halflife_elf_end
global memory_elf_start, memory_elf_end
global sokoban_elf_start, sokoban_elf_end
global battleship_elf_start, battleship_elf_end
global pig_elf_start, pig_elf_end
global raycast_elf_start, raycast_elf_end
global tron_elf_start, tron_elf_end
global spaceinv_elf_start, spaceinv_elf_end
global asteroids_elf_start, asteroids_elf_end
global flappy_elf_start, flappy_elf_end
global gb_elf_start, gb_elf_end
global lander_elf_start, lander_elf_end
global yahtzee_elf_start, yahtzee_elf_end
global checkers_elf_start, checkers_elf_end
global gomoku_elf_start, gomoku_elf_end
global frogger_elf_start, frogger_elf_end
global chess_elf_start, chess_elf_end
global vpoker_elf_start, vpoker_elf_end
global mancala_elf_start, mancala_elf_end
global dotsbox_elf_start, dotsbox_elf_end
global missile_elf_start, missile_elf_end
global pacman_elf_start, pacman_elf_end
global solitaire_elf_start, solitaire_elf_end
global gems_elf_start, gems_elf_end
global columns_elf_start, columns_elf_end
global freecell_elf_start, freecell_elf_end
global spider_elf_start, spider_elf_end
global sandbox_elf_start, sandbox_elf_end
global forth_elf_start, forth_elf_end
global cc_elf_start, cc_elf_end
global crash_elf_start, crash_elf_end
global futex_elf_start, futex_elf_end

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

timer_elf_start:
    incbin "build/timer.elf"
timer_elf_end:

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

bj_elf_start:
    incbin "build/bj.elf"
bj_elf_end:

typing_elf_start:
    incbin "build/typing.elf"
typing_elf_end:

simon_elf_start:
    incbin "build/simon.elf"
simon_elf_end:

c4_elf_start:
    incbin "build/c4.elf"
c4_elf_end:

wordle_elf_start:
    incbin "build/wordle.elf"
wordle_elf_end:

gfxdemo_elf_start:
    incbin "build/gfxdemo.elf"
gfxdemo_elf_end:

scene3d_elf_start:
    incbin "build/scene3d.elf"
scene3d_elf_end:

terrain_elf_start:
    incbin "build/terrain.elf"
terrain_elf_end:

demoscene_elf_start:
    incbin "build/demoscene.elf"
demoscene_elf_end:

doom_elf_start:
    incbin "build/doom.elf"
doom_elf_end:

quake_elf_start:
    incbin "build/quake.elf"
quake_elf_end:

nes_elf_start:
    incbin "build/nes.elf"
nes_elf_end:

reversi_elf_start:
    incbin "build/reversi.elf"
reversi_elf_end:

lights_elf_start:
    incbin "build/lights.elf"
lights_elf_end:

fifteen_elf_start:
    incbin "build/fifteen.elf"
fifteen_elf_end:

mastermind_elf_start:
    incbin "build/mastermind.elf"
mastermind_elf_end:

pong_elf_start:
    incbin "build/pong.elf"
pong_elf_end:

halflife_elf_start:
    incbin "build/halflife.elf"
halflife_elf_end:

memory_elf_start:
    incbin "build/memory.elf"
memory_elf_end:

sokoban_elf_start:
    incbin "build/sokoban.elf"
sokoban_elf_end:

battleship_elf_start:
    incbin "build/battleship.elf"
battleship_elf_end:

pig_elf_start:
    incbin "build/pig.elf"
pig_elf_end:

raycast_elf_start:
    incbin "build/raycast.elf"
raycast_elf_end:

tron_elf_start:
    incbin "build/tron.elf"
tron_elf_end:

spaceinv_elf_start:
    incbin "build/spaceinv.elf"
spaceinv_elf_end:

asteroids_elf_start:
    incbin "build/asteroids.elf"
asteroids_elf_end:

flappy_elf_start:
    incbin "build/flappy.elf"
flappy_elf_end:

gb_elf_start:
    incbin "build/gb.elf"
gb_elf_end:

lander_elf_start:
    incbin "build/lander.elf"
lander_elf_end:

yahtzee_elf_start:
    incbin "build/yahtzee.elf"
yahtzee_elf_end:

checkers_elf_start:
    incbin "build/checkers.elf"
checkers_elf_end:

gomoku_elf_start:
    incbin "build/gomoku.elf"
gomoku_elf_end:

frogger_elf_start:
    incbin "build/frogger.elf"
frogger_elf_end:

chess_elf_start:
    incbin "build/chess.elf"
chess_elf_end:

vpoker_elf_start:
    incbin "build/vpoker.elf"
vpoker_elf_end:

mancala_elf_start:
    incbin "build/mancala.elf"
mancala_elf_end:

dotsbox_elf_start:
    incbin "build/dotsbox.elf"
dotsbox_elf_end:

missile_elf_start:
    incbin "build/missile.elf"
missile_elf_end:

pacman_elf_start:
    incbin "build/pacman.elf"
pacman_elf_end:

solitaire_elf_start:
    incbin "build/solitaire.elf"
solitaire_elf_end:

gems_elf_start:
    incbin "build/gems.elf"
gems_elf_end:

columns_elf_start:
    incbin "build/columns.elf"
columns_elf_end:

freecell_elf_start:
    incbin "build/freecell.elf"
freecell_elf_end:

spider_elf_start:
    incbin "build/spider.elf"
spider_elf_end:

sandbox_elf_start:
    incbin "build/sandbox.elf"
sandbox_elf_end:

forth_elf_start:
    incbin "build/forth.elf"
forth_elf_end:

cc_elf_start:
    incbin "build/cc.elf"
cc_elf_end:

crash_elf_start:
    incbin "build/crash.elf"
crash_elf_end:

futex_elf_start:
    incbin "build/futex.elf"
futex_elf_end:

section .note.GNU-stack noalloc noexec nowrite progbits
