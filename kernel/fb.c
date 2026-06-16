/*
 * fb.c — linear framebuffer graphics over QEMU's std VGA (Bochs VBE).
 *
 * VGA text mode is a grid of characters; a *framebuffer* is a flat array of
 * pixels you draw anything into. We ask QEMU's display adapter (the "Bochs VBE"
 * interface, via I/O ports 0x1CE/0x1CF) for a 32-bit-per-pixel linear mode,
 * find the framebuffer's physical address in the VGA card's PCI BAR0, map it,
 * and write pixels: each is 0x00RRGGBB.
 *
 * Text is rasterized with the 8x16 bitmap font in font.c (font_glyphs).
 */
#include "fb.h"
#include "font.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "io.h"
#include "string.h"
#include "vfs.h"

#define VBE_INDEX  0x01CE
#define VBE_DATA   0x01CF
#define VBE_XRES   1
#define VBE_YRES   2
#define VBE_BPP    3
#define VBE_ENABLE 4
#define VBE_ENABLED      0x01
#define VBE_LFB_ENABLED  0x40

static volatile uint32_t *lfb;
static uint32_t *target;      /* if set, draw here instead of the live screen */
static int fb_w, fb_h;

static void vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_INDEX, index);
    outw(VBE_DATA, value);
}

int fb_init(uint16_t width, uint16_t height) {
    pci_device_t vga = pci_find(0x1234, 0x1111);   /* QEMU std VGA */
    if (!vga.valid)
        return -1;

    vbe_write(VBE_ENABLE, 0);
    vbe_write(VBE_XRES, width);
    vbe_write(VBE_YRES, height);
    vbe_write(VBE_BPP, 32);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);

    uint64_t base = pci_bar(&vga, 0);               /* BAR0 = linear framebuffer */
    uint64_t bytes = (uint64_t)width * height * 4;
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE)
        vmm_map(base + off, base + off, PTE_WRITABLE);
    lfb = (volatile uint32_t *)(uintptr_t)base;
    fb_w = width;
    fb_h = height;
    return 0;
}

int fb_width(void)  { return fb_w; }
int fb_height(void) { return fb_h; }

void fb_pixel(int x, int y, uint32_t color) {
    if ((unsigned)x < (unsigned)fb_w && (unsigned)y < (unsigned)fb_h) {
        if (target) target[y * fb_w + x] = color;
        else        lfb[y * fb_w + x]    = color;
    }
}

uint32_t fb_get_pixel(int x, int y) {
    if ((unsigned)x < (unsigned)fb_w && (unsigned)y < (unsigned)fb_h)
        return (target ? target[y * fb_w + x] : lfb[y * fb_w + x]) & 0xFFFFFF;
    return 0;
}

void fb_set_target(uint32_t *backbuffer) {
    target = backbuffer;
}

void fb_present(void) {
    if (!target) return;
    /* memcpy, not a per-element loop: lfb is volatile, so an element loop can't
     * be vectorised — and this runs on every scene change (drags, typing). */
    memcpy((void *)lfb, target, (size_t)fb_w * fb_h * 4);
}

/* Save a screenshot of the live screen to `name` as a 24-bit BMP, downscaled to
 * at most SHOT_W x SHOT_H (every Nth pixel) so the file stays small (~576 KB) and
 * the write is light. Returns 0 on success, -1 on failure. */
#define SHOT_W 512
#define SHOT_H 384
static uint8_t g_shot[54 + SHOT_W * SHOT_H * 3];   /* BMP header + 24-bit pixels (BSS) */
int fb_save_bmp(const char *name) {
    if (!lfb || fb_w < 1 || fb_h < 1) return -1;
    const volatile uint32_t *src = lfb;   /* the live presented screen (a complete frame), not the WIP back buffer */
    int ow = fb_w < SHOT_W ? fb_w : SHOT_W;
    int oh = fb_h < SHOT_H ? fb_h : SHOT_H;
    int sx = fb_w / ow, sy = fb_h / oh;            /* integer downscale step */
    int row = ow * 3;                              /* 24-bit; ow is even so the row is 4-aligned */
    long datasz = (long)row * oh, total = 54 + datasz;
    uint8_t *h = g_shot;
    for (int i = 0; i < 54; i++) h[i] = 0;
    h[0] = 'B'; h[1] = 'M';
    h[2] = total; h[3] = total >> 8; h[4] = total >> 16; h[5] = total >> 24;
    h[10] = 54;                                    /* pixel-data offset */
    h[14] = 40;                                    /* DIB (BITMAPINFOHEADER) size */
    h[18] = ow; h[19] = ow >> 8; h[20] = ow >> 16; h[21] = ow >> 24;
    h[22] = oh; h[23] = oh >> 8; h[24] = oh >> 16; h[25] = oh >> 24;
    h[26] = 1;                                     /* planes */
    h[28] = 24;                                    /* bits per pixel */
    h[34] = datasz; h[35] = datasz >> 8; h[36] = datasz >> 16; h[37] = datasz >> 24;
    uint8_t *px = g_shot + 54;
    for (int oy = 0; oy < oh; oy++) {
        int iy = (oh - 1 - oy) * sy;               /* BMP scanlines are bottom-up */
        if (iy >= fb_h) iy = fb_h - 1;
        uint8_t *r = px + (long)oy * row;
        for (int ox = 0; ox < ow; ox++) {
            int ix = ox * sx; if (ix >= fb_w) ix = fb_w - 1;
            uint32_t c = src[(long)iy * fb_w + ix];   /* 0x00RRGGBB */
            r[ox*3+0] = c & 0xff;                  /* B */
            r[ox*3+1] = (c >> 8) & 0xff;           /* G */
            r[ox*3+2] = (c >> 16) & 0xff;          /* R */
        }
    }
    return vfs_write(name, g_shot, (unsigned long)total) < 0 ? -1 : 0;
}

/* Copy just a clipped rectangle from the back buffer to the visible screen.
 * Used by the compositor to flush a small dirty area (e.g. the cursor) instead
 * of the whole ~3 MB framebuffer. */
void fb_present_rect(int x, int y, int w, int h) {
    if (!target) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        size_t row = (size_t)(y + j) * fb_w + x;
        memcpy((void *)(lfb + row), target + row, (size_t)w * 4);
    }
}

void fb_clear(uint32_t color) {
    for (int i = 0; i < fb_w * fb_h; i++)
        lfb[i] = color;
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    /* Clip the rectangle and resolve the destination ONCE, then fill with a
     * tight loop — instead of a bounds-checked, target-branching fb_pixel per
     * pixel. This is the hottest draw primitive (wallpaper, every window/title/
     * taskbar fill), run on every scene change. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    uint32_t *dst = target ? target : (uint32_t *)lfb;
    for (int j = 0; j < h; j++) {
        uint32_t *row = dst + (size_t)(y + j) * fb_w + x;
        for (int i = 0; i < w; i++) row[i] = color;
    }
}

void fb_glyph(int x, int y, char c, uint32_t fg, uint32_t bg) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 128) uc = '?';
    const unsigned char *g = font_glyphs[uc];
    /* fast path: the whole glyph is on-screen → write directly, no per-pixel
     * bounds test or target branch. This is the hot path for window text. */
    if (x >= 0 && y >= 0 && x + font_width <= fb_w && y + font_height <= fb_h) {
        uint32_t *dst = target ? target : (uint32_t *)lfb;
        for (int row = 0; row < font_height; row++) {
            uint32_t *p = dst + (size_t)(y + row) * fb_w + x;
            unsigned bits = g[row];
            for (int col = 0; col < font_width; col++)
                p[col] = (bits & (0x80 >> col)) ? fg : bg;
        }
        return;
    }
    for (int row = 0; row < font_height; row++)            /* clipped fallback */
        for (int col = 0; col < font_width; col++)
            fb_pixel(x + col, y + row, (g[row] & (0x80 >> col)) ? fg : bg);
}

/* Transparent glyph: paint only the set pixels in `fg`, leaving the background
 * untouched. Used for UI labels drawn over an existing scene. */
void fb_glyph_fg(int x, int y, char c, uint32_t fg) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 128) uc = '?';
    const unsigned char *g = font_glyphs[uc];
    if (x >= 0 && y >= 0 && x + font_width <= fb_w && y + font_height <= fb_h) {
        uint32_t *dst = target ? target : (uint32_t *)lfb;
        for (int row = 0; row < font_height; row++) {
            uint32_t *p = dst + (size_t)(y + row) * fb_w + x;
            unsigned bits = g[row];
            for (int col = 0; col < font_width; col++)
                if (bits & (0x80 >> col)) p[col] = fg;
        }
        return;
    }
    for (int row = 0; row < font_height; row++)            /* clipped fallback */
        for (int col = 0; col < font_width; col++)
            if (g[row] & (0x80 >> col)) fb_pixel(x + col, y + row, fg);
}

void fb_text(int x, int y, const char *s, uint32_t color, int scale) {
    for (int i = 0; s[i]; i++) {
        unsigned char uc = (unsigned char)s[i];
        if (uc >= 128) uc = '?';
        const unsigned char *g = font_glyphs[uc];
        for (int row = 0; row < font_height; row++)
            for (int col = 0; col < font_width; col++)
                if (g[row] & (0x80 >> col))
                    fb_fill_rect(x + i * font_width * scale + col * scale,
                                 y + row * scale, scale, scale, color);
    }
}

void fb_scroll(int px, uint32_t bg) {
    for (int y = 0; y < fb_h - px; y++)
        for (int x = 0; x < fb_w; x++)
            lfb[y * fb_w + x] = lfb[(y + px) * fb_w + x];
    for (int y = fb_h - px; y < fb_h; y++)
        for (int x = 0; x < fb_w; x++)
            lfb[y * fb_w + x] = bg;
}
