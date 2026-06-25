/* gdbstub.h — GDB remote-serial-protocol stub (M1204). Enabled by `-append
 * gdbstub`; kmain then int3's and the #BP handler serves gdb over COM2. */
#pragma once

struct registers;

void gdbstub_arm(void);                      /* enable the stub (multiboot cmdline) */
int  gdbstub_armed(void);                    /* is the stub enabled? */
void gdbstub_serve(struct registers *r);     /* RSP loop on COM2 from the trap frame */
