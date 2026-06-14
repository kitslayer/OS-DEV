# Milestone 137 — keyboard-drivable Apps menu (F9)

**Goal:** the last mouse-only hole in the desktop. Window management was already
keyboard-driven (F2 cycle, F3 minimize, F4 maximize, F5/F6 tile), and the browser,
shell, and Files window all take keys — but **launching an app** still required
clicking the taskbar's *Apps* button and then a menu item. So apps like the
Monitor or the games couldn't be opened without a mouse (which also meant they
couldn't be opened in headless tests without a temporary auto-open hack).

## How

- **F9** (scancode `0x43` → WM control code `0x19`, intercepted before app key
  routing like the other F-keys) toggles the Apps menu and resets the highlighted
  item to the top.
- While the menu is open it has keyboard focus: **up/down** move the highlight
  (`menu_sel`, wrapping), **Enter** launches `menu[menu_sel]` via the same
  `spawn_app` the mouse path uses, and **Esc** closes it. All keys are swallowed
  while the menu is up (they don't leak to the focused window).
- The menu render highlights `menu_sel` with a filled bar + white text.

The mouse path (click *Apps*, click an item) is unchanged.

## Verified

Headless, keyboard-only: **F9** opens the menu (all 15 items listed, *Apps* button
lit), **down**-arrows move the highlight to *Clock*, **Enter** launches it — a
Clock window opens showing the live time, RAM, uptime, and task count. No panics.
This also retires the temporary auto-open trick used to test the Monitor: every
app is now launchable in a headless screenshot test.

## Files
- `kernel/keyboard.c` — F9 (`0x43`) → control code `0x19`
- `kernel/desktop.c` — `menu_sel`; F9 toggles the menu; up/down/Enter/Esc drive it
  while open; the menu render highlights the selection; Welcome hint updated
