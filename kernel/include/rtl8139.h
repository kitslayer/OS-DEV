/* rtl8139.h — Realtek RTL8139 ("8139too") 10/100 PCI NIC driver.
 *
 * A second supported network card, alongside the Intel e1000. Exposes the same
 * poll-based interface the e1000 driver does (init / mac / send / receive) so the
 * NIC-agnostic stack in net.c can drive either one through the nic.c dispatcher. */
#pragma once
#include <stdint.h>

/* Probe + bring up the card: enable bus-mastering, reset, set up the RX ring
 * and TX buffers, read the MAC. Returns 0 on success, -1 if no RTL8139 present. */
int  rtl8139_init(void);

const uint8_t *rtl8139_mac(void);              /* our 6-byte hardware address */

int  rtl8139_send(const void *frame, uint16_t len);        /* 0 on success   */
int  rtl8139_receive(void *out, uint16_t max);             /* len, or 0 none */
