# Game Boy — a Nintendo Game Boy emulator app

Built the same way as the NES app: vendor a single-header core + a thin platform
shim + a freely-licensed game on the FAT disk.

- **`peanut_gb.h`** — [Peanut-GB](https://github.com/deltabeard/Peanut-GB), an
  **MIT** single-header DMG Game Boy emulator (CPU + PPU + the common MBCs),
  vendored verbatim.
- **`gb_osdev.c`** — the OS-DEV shim (our code): reads the chosen `.gb` off the
  disk, serves it to Peanut-GB through its ROM / cart-RAM callbacks, maps each
  2-bit LCD shade to the classic DMG green into a 160×144 framebuffer
  (`sys_gfx_blit`), and feeds the joypad byte (active-low) from raw PS/2
  scancodes — with the same same-frame-tap latch as the NES so quick taps and
  held keys both register. A `.gb` picker appears when more than one ROM is on
  disk; with one it launches straight in.

## The game

Ships **Libbet and the Magic Floor** (`tools/libbet.gb`, **Zlib** licensed, by
Damian Yerrick) — a real Game Boy puzzle game. The emulator is ROM-agnostic:
drop any supported `.gb` on the disk and it appears in the picker.

## Controls

Arrows = D-pad · **X** = A · **Z** = B · **Enter** = Start · **Shift** =
Select · **Esc** = quit. Launch with `run gb` or Apps → Game Boy.
