# 308–326: the app explosion + per-app colour

After the CSS arc ([docs/304](304-css.md)), this stretch turned the OS from "ten
mostly-monochrome apps" into a colourful suite of **seventeen**, and added a
framework that let every windowed program tint its own text.

## The apps are ring-3 ELF programs

Each app under `user/*.c` is a freestanding program: compiled
`-mgeneral-regs-only` (no FPU/SSE), linked with `user/ulib.c` against
`user/user.ld`, built to its own `build/<name>.elf`, `incbin`'d into the kernel
by `kernel/asm/user_blob.asm`, and registered in `kernel/app.c`'s `progs[]`
table (so it's launchable via `run <name>` or the F9 Apps menu). They talk to the
kernel only through the small syscall set in `user/ulib.h` — there is **no shared
memory with the kernel**, so an app bug stays in its own ring-3 address space.
They draw by printing into a 44×17 character grid (`sys_clear` + `print`), read
input with the non-blocking `sys_pollkey` (real-time games) or the blocking
`readline` (turn-based / terminal apps), and can beep the speaker (`sys_beep`).

New programs this arc:

- **sudoku** — a logic puzzle: cursor + `1`–`9`, row/column/box conflict
  detection, fixed clues.
- **calendar** — a month view from the RTC, weekday via Zeller's congruence,
  arrow-key paging.
- **mandel** — an escape-time Mandelbrot in **Q16.16 fixed-point** (no FPU),
  arrow-pan + `+/-` zoom.
- **piano** — the keyboard plays notes on the speaker (the home row = white keys,
  the row above = black keys, `z`/`x` octave); an equal-tempered frequency table.
- **maze** — an iterative recursive-backtracker maze, walked to the exit.
- **adv** — a parser-driven text adventure (a six-room dungeon) using `readline`.
- **matrix** — a "digital rain" screensaver, the first ambient animation.

## M311: per-cell colour, additively

Apps had always rendered in one green (`0x33FF66`, hard-coded in `app_render`).
M311 added a **16-entry palette** and a `sys_setcolor(idx)` syscall that sets the
colour applied to subsequently-printed characters. The key design choice was to
make it **purely additive**:

- a parallel `uint8_t gcol[APP_ROWS][APP_COLS]` sits beside the existing
  `char grid[][]`, written in `grid_putc` from the app's current colour;
- `app_render` looks up `app_palette[gcol[..] & 15]`;
- **palette index 0 is the original green**, and a fresh app's `gcol`/`curcol`
  start at 0 (`kzalloc`).

So an app that never calls `sys_setcolor` renders **byte-identically** to before
— the shell, and any unported app, are untouched. The review confirmed every
`gcol[i][j]` sits beside an already-proven `grid[i][j]` (parallel arrays, same
indices), the palette index is `&15`-masked at write and read, and the worst
failure is a wrong colour, never an out-of-bounds. Scrollback stays green (the
live colours scroll with their rows; scrollback colour is deliberately dropped to
avoid any extra bookkeeping).

## Colourising the apps

With the syscall in place, each app sets colours as it draws — per character
where it matters:

- **Mandelbrot**: escape bands coloured by escape speed.
- **Sudoku**: cyan cursor, red conflicts, white clues, grey borders.
- **2048**: tiles by value (2 white … 2048 lime).
- **Minesweeper**: the classic number hues (1 blue, 2 green, 3 red, …).
- **Tetris**: each tetromino its standard colour (I cyan, O yellow, …) — the
  board stores the piece index, so locked pieces keep their colour.
- **Breakout**: rainbow brick rows; **Snake**: red food / yellow head / lime body.
- **clock / calc / editor / life**: coloured dashboards, results, status bars, cells.

Per-character colour means printing cell-by-cell (`sys_setcolor` then a one-char
`print`/`sys_write`) instead of one big buffer — a few hundred syscalls per
frame, which is nothing for an interactive app.

## Files

- `kernel/include/syscall.h` (`SYS_setcolor`), `kernel/syscall.c` (dispatch),
  `kernel/app.c` (`gcol`/`curcol`, `app_palette`, `app_setcolor`, the
  `grid_putc`/`grid_scroll`/`grid_clear`/`app_render` changes),
  `user/ulib.{h,c}` (`sys_setcolor`).
- `user/<name>.c` for each app; `Makefile` + `kernel/asm/user_blob.asm` +
  `kernel/app.c` `progs[]` for each registration.
