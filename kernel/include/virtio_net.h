/*
 * virtio_net.h — virtio-net paravirtual NIC (the standard fast VM network card).
 *
 * The third member of the virtio trio (alongside kernel/virtio_blk.c). Where
 * kernel/e1000.c and kernel/rtl8139.c drive real emulated silicon register by
 * register, virtio-net is the NIC a hypervisor *offers* directly: guest and host
 * share two virtqueues of descriptors in RAM (queue 0 = receive, queue 1 =
 * transmit) and exchange Ethernet frames through them. We speak the LEGACY
 * (virtio 0.9.5) transport over PCI — a plain I/O-port BAR for the config
 * registers and split virtqueues whose pages are handed to the device by
 * page-frame number — exactly like kernel/virtio_blk.c. Discovered on the PCI
 * bus as vendor 0x1AF4, device 0x1000.
 *
 * It exposes the same poll-based interface the e1000/rtl8139 drivers do
 * (init / mac / send / poll-receive) so the NIC-agnostic stack in net.c drives
 * it through the nic.c dispatcher with no stack changes. With a virtio-net
 * device present and no e1000, the whole stack runs over virtio-net; if no
 * virtio NIC is attached, virtio_net_init() is a clean no-op and the kernel
 * boots exactly as before.
 */
#pragma once
#include <stdint.h>

/* Probe the PCI bus for a legacy virtio network device and bring its RX/TX
 * virtqueues up (reset -> ACK -> DRIVER -> negotiate VIRTIO_NET_F_MAC ->
 * FEATURES_OK -> queues -> DRIVER_OK), pre-filling the RX queue with receive
 * buffers. Returns 0 if a usable device was initialized, -1 if none is present
 * (a clean no-op). Idempotent: a second call after success just returns 0. */
int virtio_net_init(void);

/* 1 if virtio_net_init() brought a device up, else 0. */
int virtio_net_present(void);

const uint8_t *virtio_net_get_mac(void);       /* our 6-byte hardware address */

/* Transmit one Ethernet frame (a virtio_net_hdr is prepended internally).
 * Returns 0 on success, -1 on bad-arg / no device / timeout. */
int virtio_net_send(const void *frame, uint16_t len);

/* Poll the RX virtqueue for one received Ethernet frame. If one is ready, copy
 * it (the 10-byte virtio_net_hdr stripped) into `out`, clamped to BOTH `max` and
 * the device-reported length, recycle the buffer, and return the frame length.
 * Returns 0 if nothing is waiting. */
int virtio_net_poll_receive(void *out, uint16_t max);

/* Boot-time log: if a virtio NIC is present, print the MAC read from its config
 * space. No-op (logs nothing of substance) if no device. */
void virtio_net_selftest(void);
