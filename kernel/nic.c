/*
 * nic.c — NIC dispatcher: pick a supported network card and route the stack's
 * send/receive/mac through that driver.
 *
 * The stack in net.c is card-agnostic; this is the one place that knows which
 * concrete drivers exist. At bring-up we probe each supported card in priority
 * order and bind a set of function pointers to the first one that initialises.
 * net.c then calls nic_send / nic_receive / nic_mac and the bytes flow over
 * whichever card answered — the e1000 today, the RTL8139 when that's the only
 * card present (or a future third card with one more probe line here).
 *
 * Priority when several cards are present: e1000 first. It's the richer gigabit
 * part, it's what the headless test suite boots with, and preferring it keeps
 * the default path byte-identical to before this dispatcher existed. With only
 * an RTL8139 on the bus, the whole stack runs over the RTL8139; with only a
 * virtio-net device, it runs over virtio-net (the paravirtual NIC).
 */
#include "nic.h"
#include "e1000.h"
#include "rtl8139.h"
#include "virtio_net.h"

/* The bound driver: each field points at the active card's implementation. NULL
 * driver => nic_init() found no supported card (every call no-ops safely). */
static const uint8_t *(*drv_mac)(void);
static int            (*drv_send)(const void *frame, uint16_t len);
static int            (*drv_receive)(void *out, uint16_t max);
static const char     *drv_name = "none";

int nic_init(void) {
    /* e1000 first (preferred when both are present). */
    if (e1000_init() == 0) {
        drv_mac     = e1000_mac;
        drv_send    = e1000_send;
        drv_receive = e1000_receive;
        drv_name    = "e1000";
        return 0;
    }
    /* Otherwise the RTL8139, if present. */
    if (rtl8139_init() == 0) {
        drv_mac     = rtl8139_mac;
        drv_send    = rtl8139_send;
        drv_receive = rtl8139_receive;
        drv_name    = "rtl8139";
        return 0;
    }
    /* Otherwise the paravirtual virtio-net NIC, if present. */
    if (virtio_net_init() == 0) {
        drv_mac     = virtio_net_get_mac;
        drv_send    = virtio_net_send;
        drv_receive = virtio_net_poll_receive;
        drv_name    = "virtio-net";
        return 0;
    }
    return -1;                /* no supported NIC on the bus */
}

const char *nic_name(void) { return drv_name; }

static const uint8_t zero_mac[6] = {0};
const uint8_t *nic_mac(void) {
    return drv_mac ? drv_mac() : zero_mac;
}

int nic_send(const void *frame, uint16_t len) {
    return drv_send ? drv_send(frame, len) : -1;
}

int nic_receive(void *out, uint16_t max) {
    return drv_receive ? drv_receive(out, max) : 0;
}
