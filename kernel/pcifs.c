/*
 * pcifs.c — expose PCI enumeration as a navigable file tree under /pci (M1120).
 *
 * The "everything is a file" treatment of the bus probe that AHCI/rtl8139/virtio
 * already rely on: `cat /pci` lists every device, and /pci/<bb:ss.f>/<field>
 * reads one device's vendor/device/class/irq/bars or a hexdump of its config
 * space. Read-only; the VFS routes /pci here (see vfs.c). The principled form of
 * the flat `lspci` text blob.
 */
#include "pci.h"

static int sa(char *b, int p, int max, const char *s) { while (*s && p < max - 1) b[p++] = *s++; return p; }
static int hx(char *b, int p, int max, uint32_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) { int d = (int)((v >> (i * 4)) & 0xF); if (p < max - 1) b[p++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
    return p;
}
static int dc(char *b, int p, int max, uint32_t v) {
    char t[12]; int n = 0; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < max - 1) b[p++] = t[--n]; return p;
}
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* parse "bb:ss.f" -> bus/slot/func; returns the field-name pointer (after '/'), or 0 */
static const char *parse_bdf(const char *s, uint8_t *bus, uint8_t *slot, uint8_t *func) {
    int h, l;
    if ((h = hexv(s[0])) < 0 || (l = hexv(s[1])) < 0 || s[2] != ':') return 0; *bus = (uint8_t)(h * 16 + l); s += 3;
    if ((h = hexv(s[0])) < 0 || (l = hexv(s[1])) < 0 || s[2] != '.') return 0; *slot = (uint8_t)(h * 16 + l); s += 3;
    if ((h = hexv(s[0])) < 0) return 0; *func = (uint8_t)h; s += 1;
    if (*s != '/') return 0;
    return s + 1;                                       /* the field name */
}
static int feq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

int pcifs_read(const char *sub, char *buf, int max) {
    if (!sub || !sub[0]) {                              /* /pci -> list every device */
        pci_device_t d[64]; int n = pci_collect(d, 64); if (n > 64) n = 64;
        int p = sa(buf, 0, max, "  BDF       VEND:DEV   CLASS\n");
        for (int i = 0; i < n; i++) {
            p = sa(buf, p, max, "  ");
            p = hx(buf, p, max, d[i].bus, 2);  p = sa(buf, p, max, ":");
            p = hx(buf, p, max, d[i].slot, 2); p = sa(buf, p, max, ".");
            p = dc(buf, p, max, d[i].func);    p = sa(buf, p, max, "  ");
            p = hx(buf, p, max, d[i].vendor_id, 4); p = sa(buf, p, max, ":"); p = hx(buf, p, max, d[i].device_id, 4);
            p = sa(buf, p, max, "  ");
            p = hx(buf, p, max, d[i].class_id, 2); p = sa(buf, p, max, ":"); p = hx(buf, p, max, d[i].subclass, 2); p = sa(buf, p, max, ":"); p = hx(buf, p, max, d[i].prog_if, 2);
            p = sa(buf, p, max, "\n");
        }
        if (p < max) buf[p] = 0; return p;
    }

    uint8_t bus, slot, func;
    const char *field = parse_bdf(sub, &bus, &slot, &func);
    if (!field) return -1;
    if ((pci_read32(bus, slot, func, 0) & 0xFFFF) == 0xFFFF) return -1;   /* no such device */

    int p = 0;
    if (feq(field, "vendor")) { p = sa(buf, p, max, "0x"); p = hx(buf, p, max, pci_read32(bus, slot, func, 0) & 0xFFFF, 4); }
    else if (feq(field, "device")) { p = sa(buf, p, max, "0x"); p = hx(buf, p, max, (pci_read32(bus, slot, func, 0) >> 16) & 0xFFFF, 4); }
    else if (feq(field, "class")) {
        uint32_t c = pci_read32(bus, slot, func, 8);
        p = hx(buf, p, max, (c >> 24) & 0xFF, 2); p = sa(buf, p, max, ":"); p = hx(buf, p, max, (c >> 16) & 0xFF, 2); p = sa(buf, p, max, ":"); p = hx(buf, p, max, (c >> 8) & 0xFF, 2);
    }
    else if (feq(field, "irq")) { p = dc(buf, p, max, pci_read32(bus, slot, func, 0x3C) & 0xFF); }
    else if (feq(field, "bars")) {
        for (int i = 0; i < 6; i++) { p = sa(buf, p, max, "bar"); p = dc(buf, p, max, (uint32_t)i); p = sa(buf, p, max, "=0x"); p = hx(buf, p, max, pci_read32(bus, slot, func, 0x10 + 4 * i), 8); p = sa(buf, p, max, "\n"); }
    }
    else if (feq(field, "config")) {                    /* hexdump the 256-byte config space */
        for (int off = 0; off < 256; off += 16) {
            p = hx(buf, p, max, (uint32_t)off, 2); p = sa(buf, p, max, ": ");
            for (int j = 0; j < 16; j += 4) { uint32_t w = pci_read32(bus, slot, func, off + j); p = hx(buf, p, max, w, 8); p = sa(buf, p, max, " "); }
            p = sa(buf, p, max, "\n");
        }
    }
    else return -1;
    p = sa(buf, p, max, "\n");
    if (p < max) buf[p] = 0; return p;
}
