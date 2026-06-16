//
// i_sound_osdev.c — DOOM sound effects for this OS.
//
// Implements an "osdev" sound_module_t: it decodes the WAD's DMX sound lumps,
// keeps up to 8 mixing channels, mixes them down to 48 kHz interleaved
// 16-bit stereo, and pushes the mix into the kernel's streaming PCM ring via
// sys_pcm_stream() / sys_pcm_avail() (both non-blocking).
//
// The pump (osdev_sound_pump) is called once per rendered frame from the
// platform layer (doomgeneric_osdev.c, top of DG_DrawFrame), which keeps the
// ~1-second ring topped up at ~70 fps without ever blocking.
//
// Music is NOT implemented here — DOOM's MUS format needs an OPL/FM synth we
// don't have. i_sound.c keeps its no-op music stubs and no music module is
// selected (snd_musicdevice can stay whatever; InitMusicModule does nothing
// unless FEATURE_SOUND, which is off).
//

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"

// ---- syscalls from ulib (declared here; resolved at link against user_ulib.o,
// matching the pattern in doomgeneric_osdev.c — ulib.h is outside our -I path).
extern int sys_pcm_stream(const void *frames, int nframes);
extern int sys_pcm_avail(void);

// Output format of the kernel ring.
#define OUT_RATE   48000
#define MIX_CHUNK  512          // frames mixed/pushed per pump iteration

#define NUM_CHANNELS 8

// A decoded sfx, cached in sfxinfo_t->driver_data.
typedef struct
{
    int16_t *samples;           // mono 16-bit PCM (malloc'd)
    int      length;            // number of samples
    int      rate;              // original sample rate (Hz)
} osdev_sfx_t;

// One active mixing channel.
typedef struct
{
    const int16_t *samples;     // -> osdev_sfx_t.samples (not owned)
    int      length;            // sample count
    uint32_t pos;               // 16.16 fixed-point read cursor into samples
    uint32_t step;              // 16.16 fixed-point advance per output frame
    int      vol;               // 0..127
    int      sep;               // 0..254 (128 = centre)
    int      active;
} osdev_channel_t;

static osdev_channel_t channels[NUM_CHANNELS];
static boolean sound_initialised = false;

// ---------------------------------------------------------------------------
// DMX decode
// ---------------------------------------------------------------------------

// Decode the DMX sound lump for sfxinfo (caching the result in driver_data).
// DMX format:  u16 format(==3, LE)  u16 rate(LE)  u32 length(LE)  then
// `length` unsigned-8-bit mono samples.  Convert u8 -> s16 with (u8-128)<<8.
// Returns the cached osdev_sfx_t, or NULL on failure.
static osdev_sfx_t *CacheSfx(sfxinfo_t *sfxinfo)
{
    osdev_sfx_t *sfx;
    uint8_t *data;
    int lumplen;
    uint32_t rate, length;
    int i;

    if (sfxinfo == NULL)
    {
        return NULL;
    }

    // Already decoded?
    if (sfxinfo->driver_data != NULL)
    {
        return (osdev_sfx_t *) sfxinfo->driver_data;
    }

    if (sfxinfo->lumpnum < 0)
    {
        sfxinfo->lumpnum = I_GetSfxLumpNum(sfxinfo);
    }

    lumplen = W_LumpLength(sfxinfo->lumpnum);

    // Need at least the 8-byte header.
    if (lumplen < 8)
    {
        return NULL;
    }

    data = W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
    if (data == NULL)
    {
        return NULL;
    }

    // Header (little-endian).
    // data[0..1] = format (expect 3); data[2..3] = sample rate;
    // data[4..7] = length (sample count).
    rate   = (uint32_t) data[2] | ((uint32_t) data[3] << 8);
    length = (uint32_t) data[4] | ((uint32_t) data[5] << 8)
           | ((uint32_t) data[6] << 16) | ((uint32_t) data[7] << 24);

    // Clamp the declared length against what the lump can actually hold
    // (the 8-byte header precedes the samples).  Some lumps carry the
    // DMX 16-byte lead-in/trail pad; clamping keeps us in-bounds regardless.
    if (length > (uint32_t)(lumplen - 8))
    {
        length = (uint32_t)(lumplen - 8);
    }

    if (rate == 0)
    {
        rate = 11025;           // DMX default
    }

    if (length == 0)
    {
        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }

    sfx = malloc(sizeof(osdev_sfx_t));
    if (sfx == NULL)
    {
        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }

    sfx->samples = malloc(length * sizeof(int16_t));
    if (sfx->samples == NULL)
    {
        free(sfx);
        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }

    sfx->length = (int) length;
    sfx->rate   = (int) rate;

    // u8 (unsigned) -> s16 (signed), centred at 128.
    for (i = 0; i < (int) length; ++i)
    {
        sfx->samples[i] = (int16_t)(((int) data[8 + i] - 128) << 8);
    }

    // We copied the samples out; the WAD cache entry is no longer needed.
    W_ReleaseLumpNum(sfxinfo->lumpnum);

    sfxinfo->driver_data = sfx;
    return sfx;
}

// ---------------------------------------------------------------------------
// sound_module_t implementation
// ---------------------------------------------------------------------------

static boolean I_OSDEV_InitSound(boolean use_sfx_prefix)
{
    (void) use_sfx_prefix;

    memset(channels, 0, sizeof(channels));
    sound_initialised = true;
    return true;
}

static void I_OSDEV_ShutdownSound(void)
{
    sound_initialised = false;
    memset(channels, 0, sizeof(channels));
}

// Map a sfxinfo to its WAD lump number: "ds"/"DS" prefix + name, like the
// other DOOM ports.  Falls back to a literal name lookup (link sounds etc.).
static int I_OSDEV_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfx->name));

    return W_GetNumForName(namebuf);
}

static void I_OSDEV_UpdateSoundParams(int handle, int vol, int sep)
{
    if (!sound_initialised || handle < 0 || handle >= NUM_CHANNELS)
    {
        return;
    }

    channels[handle].vol = vol;
    channels[handle].sep = sep;
}

static int I_OSDEV_StartSound(sfxinfo_t *sfxinfo, int channel,
                              int vol, int sep)
{
    osdev_sfx_t *sfx;

    if (!sound_initialised || channel < 0 || channel >= NUM_CHANNELS)
    {
        return -1;
    }

    sfx = CacheSfx(sfxinfo);
    if (sfx == NULL || sfx->length <= 0)
    {
        return -1;
    }

    channels[channel].samples = sfx->samples;
    channels[channel].length  = sfx->length;
    channels[channel].pos     = 0;
    // Resample original rate -> 48 kHz: advance this much per output frame.
    channels[channel].step    = ((uint32_t) sfx->rate << 16) / OUT_RATE;
    if (channels[channel].step == 0)
    {
        channels[channel].step = 1;
    }
    channels[channel].vol     = vol;
    channels[channel].sep     = sep;
    channels[channel].active  = 1;

    return channel;
}

static void I_OSDEV_StopSound(int handle)
{
    if (handle < 0 || handle >= NUM_CHANNELS)
    {
        return;
    }

    channels[handle].active  = 0;
    channels[handle].samples = NULL;
}

static boolean I_OSDEV_SoundIsPlaying(int handle)
{
    if (handle < 0 || handle >= NUM_CHANNELS)
    {
        return false;
    }

    if (!channels[handle].active)
    {
        return false;
    }

    return (channels[handle].pos >> 16) < (uint32_t) channels[handle].length;
}

static void I_OSDEV_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    int i;

    // Precache every sound effect up front so the first hit isn't delayed by
    // a decode.  Non-fatal if any individual lump can't be decoded.
    for (i = 0; i < num_sounds; ++i)
    {
        CacheSfx(&sounds[i]);
    }
}

// ---------------------------------------------------------------------------
// The mixer / pump
// ---------------------------------------------------------------------------

static inline int16_t clamp_s16(int32_t v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t) v;
}

// Mix `nframes` interleaved stereo frames from the active channels into `out`.
static void MixChunk(int16_t *out, int nframes)
{
    int i, c;

    memset(out, 0, nframes * 2 * sizeof(int16_t));

    for (c = 0; c < NUM_CHANNELS; ++c)
    {
        osdev_channel_t *ch = &channels[c];
        uint32_t pos, step, len;
        int vol;
        // DOOM panning: left favours low sep, right favours high sep,
        // sep 128 == centre.  Combine with per-channel volume (0..127).
        //
        // We want a single centred, full-volume sound to play near full scale,
        // while overlapping sounds rely on the int16 clamp for headroom.
        // Scale so centre (sep=128) gives gain ~= vol: the pan fraction is
        // (sep or 255-sep)/127, which is ~1.0 at centre.  Gains are then
        // applied as (sample * gain) >> 7, so gain 127 ~= unity.  Hard panning
        // can push one side a little above 127; clamp it to keep it bounded.
        int leftgain, rightgain;

        if (!ch->active || ch->samples == NULL)
        {
            continue;
        }

        pos  = ch->pos;
        step = ch->step;
        len  = (uint32_t) ch->length;
        vol  = ch->vol;

        leftgain  = (vol * (255 - ch->sep)) / 127;
        rightgain = (vol * ch->sep)         / 127;
        if (leftgain  > 127) leftgain  = 127;
        if (rightgain > 127) rightgain = 127;

        for (i = 0; i < nframes; ++i)
        {
            uint32_t idx = pos >> 16;
            int32_t s, l, r;

            if (idx >= len)
            {
                break;          // sound finished within this chunk
            }

            s = ch->samples[idx];

            // Scale sample by side gain.  /128 normalises the 0..127 gain so a
            // centred, full-volume sound passes near unity.
            l = (s * leftgain)  >> 7;
            r = (s * rightgain) >> 7;

            out[i * 2 + 0] = clamp_s16((int32_t) out[i * 2 + 0] + l);
            out[i * 2 + 1] = clamp_s16((int32_t) out[i * 2 + 1] + r);

            pos += step;
        }

        ch->pos = pos;

        // Drop the channel once we've consumed all its samples.
        if ((pos >> 16) >= len)
        {
            ch->active  = 0;
            ch->samples = NULL;
        }
    }
}

// Called once per rendered frame from DG_DrawFrame.  Mixes and pushes chunks
// while the ring has room; never blocks (sys_pcm_stream/avail are non-blocking).
void osdev_sound_pump(void)
{
    static int16_t mixbuf[MIX_CHUNK * 2];   // interleaved stereo

    if (!sound_initialised)
    {
        return;
    }

    while (sys_pcm_avail() >= MIX_CHUNK)
    {
        int pushed;

        MixChunk(mixbuf, MIX_CHUNK);

        pushed = sys_pcm_stream(mixbuf, MIX_CHUNK);
        // If the ring couldn't take a full chunk (shouldn't happen given the
        // avail() check, but be safe), stop pumping this frame.
        if (pushed < MIX_CHUNK)
        {
            break;
        }
    }
}

// I_UpdateSound() in i_sound.c calls sound_module->Update() periodically.
// Our mixing is driven by osdev_sound_pump() from the frame loop, so Update is
// a harmless no-op (kept non-NULL so the dispatcher always has a target).
static void I_OSDEV_UpdateSound(void)
{
}

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------

// snd_sfxdevice defaults to SNDDEVICE_SB (see i_sound.c), so claim SB (plus a
// few other digital-output device ids) to ensure InitSfxModule selects us.
static snddevice_t sound_osdev_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module =
{
    sound_osdev_devices,
    sizeof(sound_osdev_devices) / sizeof(*sound_osdev_devices),
    I_OSDEV_InitSound,
    I_OSDEV_ShutdownSound,
    I_OSDEV_GetSfxLumpNum,
    I_OSDEV_UpdateSound,
    I_OSDEV_UpdateSoundParams,
    I_OSDEV_StartSound,
    I_OSDEV_StopSound,
    I_OSDEV_SoundIsPlaying,
    I_OSDEV_CacheSounds,
};
