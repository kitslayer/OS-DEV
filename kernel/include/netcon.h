/* netcon.h — network debug console (M1870). A kernel task that serves a
 * line-oriented inspection shell over TCP, for driving/debugging the machine
 * remotely when the local framebuffer/keyboard may be unavailable (real-hardware
 * bring-up). Started once from kmain after the scheduler is up. */
#pragma once
#include <stdint.h>

#define NETCON_PORT 2323

/* The console task entry (pass to task_create). Loops forever: accept one client,
 * run the command shell over the connection, close, repeat. Never returns. */
void netcon_task(void);
