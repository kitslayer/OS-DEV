# Milestone 1 — Terminal driver + `kprintf`

**Goal:** stop poking video memory by hand. Build a real scrolling terminal and
a `printf` so the rest of the kernel can just say `kprintf("x = %d\n", x)`.

## Key idea: the screen is just memory

In VGA text mode the display is an 80×25 grid living at physical `0xB8000`.
Each cell is **two bytes**: an ASCII character and an **attribute** byte
(foreground color in the low nibble, background in the high nibble). The video
hardware continuously scans that memory and paints it — there's no "draw" call.
Writing `VGA_MEM[row*80 + col] = (attr << 8) | ch` changes the screen instantly.

That address is reachable because M0 identity-mapped the low 1 GiB.

## The terminal (`kernel/vga.c`)

On top of the raw grid we track a cursor (`row`, `col`) and a current color,
and handle the control characters a terminal needs: `\n`, `\r`, `\t`, `\b`.
Two details worth knowing:

- **Scrolling is a `memcpy`.** When we fall off the bottom row we copy rows
  1..24 up over 0..23 and blank the last row. Crude, fine.
- **The blinking cursor lives in hardware, not memory.** You move it by writing
  the VGA CRT controller through **port I/O**: an index to port `0x3D4`, a value
  to `0x3D5`. This index/data port pair is the pattern *every* legacy device
  uses — you'll see it again for the PIC, PIT, and keyboard.

## Port I/O (`kernel/include/io.h`)

Devices aren't all memory-mapped; many live in a separate 64K **I/O port**
space reached with the `in`/`out` instructions. `outb`/`inb` are tiny inline-asm
wrappers, used kernel-wide.

## `kprintf` (`kernel/console.c`)

There's no libc, so we write our own `printf`. It walks the format string and
uses `<stdarg.h>` to pull arguments. Supported: `%s %c %d %u %x %X %p %%`, an
optional width and `0` pad (`%08x`), and the length modifiers **`l`** and
**`z`**.

The length modifiers are not optional polish — they're essential. In a 64-bit
kernel, addresses and sizes are 64-bit. If you `va_arg(ap, int)` a 64-bit
pointer you grab the wrong half and print garbage. `%lx`/`%lu`/`%p` read a full
64-bit argument. (That's why the test printed `feedface12345678` correctly.)

## One console, two sinks (`kernel/console.c`)

`console_putc` writes to **both** the VGA screen and the serial port (adding
`\r` before `\n` for the terminal). That's why `make run` (window) and
`make test` (headless serial) always show the same thing.

## Files
- `kernel/include/io.h` — `outb`/`inb`
- `kernel/serial.c` — the COM1 UART
- `kernel/vga.c` — the text terminal
- `kernel/console.c` — fan-out + `kprintf`
