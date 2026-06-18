# NES — a Nintendo Entertainment System emulator app

A windowed ring-3 NES emulator, built the same way as DOOM/Quake: **vendor a
portable core + a thin platform shim over our syscalls + a freely-licensed game
on the FAT disk.**

## Pieces

- **`*.c` / `*.h` (except `nes_osdev.c`)** — [libxnes](https://github.com/xboot/libxnes),
  a lightweight pure-C99 NES core (CPU/PPU/APU/DMA/controller/cartridge + the
  `mapper*.c` bank switchers), **MIT** licensed (see `LICENSE`). Vendored from
  `src/` (the `mapper/` subdir is flattened in here so the `user/nes/*.c` glob
  picks it up).
- **`nes_osdev.c`** — the OS-DEV platform layer (our code): reads the ROM off the
  FAT disk, opens a 256×240 graphics window, and runs `drain input → step one
  frame → blit`. Framebuffer pixels are libxnes's `xnes_get_pixel` (already
  `0x00RRGGBB`) straight into `sys_gfx_blit`; input is raw PS/2 scancodes
  (`sys_setkbmode(1)` + `sys_getkbevent`) mapped to joypad 1; timing is
  `sys_uptime_ms`/`sys_sleep`. Audio is a no-op sink for now (a follow-up will
  wire it to `sys_pcm_stream`, mirroring how DOOM landed video first, sound
  second).

## Local modifications to the vendored core (NOT upstream)

To run a real game we had to fix two latent bugs in libxnes; both are minimal
and isolated:

- **`mapper1.c` (`xnes_mapper1_init`)** — MMC1 now powers up with the PRG mode
  fixed to 3 (last 16K bank mapped at `$C000`), so the reset vector at `$FFFC`
  reads from the correct bank. Upstream left both PRG slots pointing at bank 0,
  so any MMC1 game booted into garbage and rendered a blank screen.
- **`cartridge.c` (NES 2.0 branch)** — a CHR-RAM cartridge (0 CHR-ROM bytes) now
  gets an 8 KB writable CHR window, matching what the iNES-1.0 branch already
  did. Without it the PPU read from a zero-length buffer.

Together these make MMC1 + CHR-RAM games (a large slice of the NES library)
work; they were verified on the host before integrating.

## The game: Nova the Squirrel

Shipped as `GAME.NES` on the FAT disk (see `tools/nova.nes`, registered in
`tools/mkfatfs.c`). **Nova the Squirrel** is an open-source (**GPLv3**) NES
platformer by NovaSquirrel — https://github.com/NovaSquirrel/NovaTheSquirrel
(mapper 1 / MMC1, 256 KB, CHR-RAM). The emulator is ROM-agnostic; drop any
supported `.nes` in as `GAME.NES` to play something else.

## Controls

Arrows = D-pad · **X** = A · **Z** = B · **Enter** = Start · **Shift** =
Select · **Esc** = quit. Launch with `run nes` or Apps → NES.
