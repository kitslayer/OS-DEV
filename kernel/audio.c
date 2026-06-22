/*
 * audio.c — audio-output dispatcher: pick the audio controller on the bus and
 * route the kernel's PCM path through that driver.
 *
 * The shape is the NIC dispatcher (nic.c) applied to sound. At bring-up we probe
 * for an Intel HD Audio controller first (the richer, modern part) and fall back
 * to AC'97 (what the default QEMU config and the headless suite boot with), then
 * bind the active driver's PCM primitives to a set of function pointers. The
 * syscall layer and the timer pump call audio_* and never name a concrete chip.
 *
 * Preferring HDA when present keeps the seam future-proof; falling back to AC'97
 * keeps the default path — and the boot/wav tests — byte-identical to before
 * this dispatcher existed (no HDA device => audio_init binds AC'97, exactly the
 * old behaviour).
 *
 * The high-level helpers — WAV decode (resample to 48 kHz stereo) and background
 * music — live HERE, built once on the primitive seam (play / stream_write /
 * pump), so both drivers share one copy and a third codec needs only its driver
 * plus one probe line in audio_init.
 */
#include "audio.h"
#include "hda.h"
#include "ac97.h"
#include "wav.h"
#include "kheap.h"
#include "timer.h"
#include <stddef.h>

/* The bound driver: each field points at the active controller's primitive. A
 * NULL play => audio_init found no supported controller (every call no-ops). */
static void (*drv_play)(const int16_t *frames, int nframes);
static void (*drv_stream_start)(void);
static void (*drv_stream_stop)(void);
static int  (*drv_stream_write)(const int16_t *frames, int nframes);
static int  (*drv_stream_avail)(void);
static void (*drv_pump)(void);
static const char *drv_name = "none";

int audio_init(void) {
    /* Intel HD Audio first (preferred when present). */
    if (hda_init() == 0) {
        drv_play         = hda_play;
        drv_stream_start = hda_stream_start;
        drv_stream_stop  = hda_stream_stop;
        drv_stream_write = hda_stream_write;
        drv_stream_avail = hda_stream_avail;
        drv_pump         = hda_pump;
        drv_name         = "hda";
        return 0;
    }
    /* Otherwise the AC'97 codec, if present. */
    if (ac97_init() == 0) {
        drv_play         = ac97_play;
        drv_stream_start = ac97_stream_start;
        drv_stream_stop  = ac97_stream_stop;
        drv_stream_write = ac97_stream_write;
        drv_stream_avail = ac97_stream_avail;
        drv_pump         = ac97_pump;
        drv_name         = "ac97";
        return 0;
    }
    return -1;                          /* no supported audio controller */
}

int  audio_ready(void)      { return drv_play != NULL; }
const char *audio_name(void){ return drv_name; }

void audio_play(const int16_t *frames, int nframes) {
    if (drv_play) drv_play(frames, nframes);
}
int audio_stream_write(const int16_t *frames, int nframes) {
    return drv_stream_write ? drv_stream_write(frames, nframes) : 0;
}
int audio_stream_avail(void) {
    return drv_stream_avail ? drv_stream_avail() : 0;
}

/* ---- shared WAV decode --------------------------------------------------- */
/* Decode a 16-bit PCM WAV into a fresh kmalloc'd 48 kHz-stereo buffer (caller
 * frees). Sets *nframes; returns NULL on a bad/unsupported WAV or OOM. Nearest-
 * neighbour resample to the 48 kHz both codecs run at. */
static int16_t *wav_decode(const uint8_t *d, int len, long *nframes) {
    int channels, rate, bits;
    long pcm_off, pcm_len;
    if (wav_parse(d, len, &channels, &rate, &bits, &pcm_off, &pcm_len) < 0) return 0;
    long in_frames = pcm_len / (2 * channels);
    const int16_t *in = (const int16_t *)(d + pcm_off);
    long out_frames = in_frames * 48000 / rate;
    if (out_frames <= 0) return 0;
    int16_t *out = kmalloc((size_t)out_frames * 4);
    if (!out) return 0;
    for (long i = 0; i < out_frames; i++) {
        long si = i * rate / 48000;                   /* nearest-neighbour */
        if (si >= in_frames) si = in_frames - 1;
        if (channels == 2) { out[i*2] = in[si*2]; out[i*2+1] = in[si*2+1]; }
        else               { out[i*2] = out[i*2+1] = in[si]; }
    }
    *nframes = out_frames;
    return out;
}

int audio_play_wav(const uint8_t *d, int len) {        /* blocking */
    if (!audio_ready()) return -1;
    long n; int16_t *pcm = wav_decode(d, len, &n);
    if (!pcm) return -1;
    audio_play(pcm, (int)n);
    kfree(pcm);
    return 0;
}

/* ---- background music ----------------------------------------------------
 * A decoded 48 kHz-stereo song the timer pump streams on its own (non-blocking)
 * into the driver's ring via drv_stream_write, so playback continues while other
 * apps run. Mutually exclusive with app streaming (the DOOM mixer's direct
 * audio_stream_write) — whichever starts last wins, matching the prior AC'97
 * semantics. The decoded buffer is owned here and freed on the next play/stop. */
static int16_t        *bg_buf;
static long            bg_frames, bg_pos;
static volatile int    bg_active;

static void play_bg(int16_t *pcm, long nframes) {
    if (!audio_ready() || !pcm || nframes <= 0) { if (pcm) kfree(pcm); return; }
    if (drv_stream_start) drv_stream_start();         /* ensure the engine runs */
    uint64_t fl; __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    int16_t *old = bg_buf;                            /* swap under cli — pump runs in IRQ */
    bg_active = 0;
    bg_buf = pcm; bg_frames = nframes; bg_pos = 0;
    bg_active = 1;
    __asm__ volatile("push %0; popfq" : : "r"(fl) : "memory", "cc");
    if (old) kfree(old);                              /* now unreferenced — safe to free */
}

void audio_stop_bg(void) {
    uint64_t fl; __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    int16_t *old = bg_buf;
    bg_active = 0; bg_buf = 0;
    __asm__ volatile("push %0; popfq" : : "r"(fl) : "memory", "cc");
    if (old) kfree(old);
}

int audio_play_wav_bg(const uint8_t *d, int len) {
    if (!audio_ready()) return -1;
    long n; int16_t *pcm = wav_decode(d, len, &n);
    if (!pcm) return -1;
    play_bg(pcm, n);                                  /* transfers ownership */
    return 0;
}

/* Timer-IRQ hook: top up the driver's software ring from the background song,
 * then run the active driver's own pump (which moves that ring into the device
 * buffers). Order matters — feed first, so the driver pump sees fresh frames. */
void audio_pump(void) {
    if (bg_active && bg_buf && drv_stream_write) {
        /* push the decoded song into the ring as space frees up; one IRQ's worth
         * of frames is bounded by the ring's free space, which stream_write
         * caps. When fully queued, mark done (buffer freed on next play/stop). */
        while (bg_pos < bg_frames) {
            int wrote = drv_stream_write(bg_buf + (size_t)bg_pos * 2, (int)(bg_frames - bg_pos));
            if (wrote <= 0) break;                    /* ring full for now */
            bg_pos += wrote;
        }
        if (bg_pos >= bg_frames) bg_active = 0;
    }
    if (drv_pump) drv_pump();
}
