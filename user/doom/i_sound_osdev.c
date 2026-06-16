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
// Music IS implemented here too (see the "MUS music synth" section below):
// DOOM's MUS-format music lumps are parsed and rendered by a small polyphonic
// triangle-wave synthesizer whose clock is driven by the audio we generate.
// The synth's per-frame sample is ADDED into the same mix as the SFX inside
// osdev_sound_pump(), so music and effects share the one 48 kHz output stream.
// DG_music_module (at the bottom of this file) exposes it to i_sound.c.
//

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

// ===========================================================================
// MUS music synth
// ---------------------------------------------------------------------------
// We parse DOOM's MUS-format music lumps and render them with a tiny
// polyphonic triangle-wave synth.  The MUS clock (140 ticks/second) is driven
// from the audio we generate: each output frame advances a fractional sample
// accumulator, and when a tick elapses we process the next group of MUS events.
// The summed voice value for each frame is produced by MusicNextSample() and
// added into the SFX mix by osdev_sound_pump() (see below), so SFX + music
// share the single 48 kHz stereo output stream.
// ===========================================================================

#define MUS_RATE        140             // MUS score runs at 140 Hz
#define MAX_VOICES      16              // hard cap on simultaneous notes
#define MUS_CHANNELS    16              // MUS has 16 logical channels (15=perc)
#define PERCUSSION_CHAN 15

// One sounding voice, keyed by (channel, note).
typedef struct
{
    int      active;
    int      channel;                   // 0..15
    int      note;                      // MIDI note 0..127
    int      vol;                       // effective per-note volume 0..127
    double   phase;                     // triangle phase, 0..1 (wraps)
    double   phase_inc;                 // phase advance per output frame
} mus_voice_t;

// A registered song: a private copy of the MUS lump plus parsed header offsets.
typedef struct
{
    uint8_t *data;                      // owned copy of the lump
    int      len;                       // copy length
    int      score_start;               // byte offset of the score
    int      score_len;                 // declared score length (bytes)
} mus_song_t;

static mus_voice_t mus_voices[MAX_VOICES];
static int  mus_chan_vol[MUS_CHANNELS]; // per-channel volume 0..127

static mus_song_t *mus_current = NULL;  // song currently selected for playback
static int   mus_pos        = 0;        // read cursor within data[] (score)
static int   mus_playing    = 0;        // 1 while actively rendering
static int   mus_paused     = 0;        // 1 while paused (clock frozen)
static int   mus_looping    = 0;        // loop at ScoreEnd?
static int   mus_finished    = 0;       // hit ScoreEnd with looping == 0

static double mus_tick_samples = 0.0;   // samples remaining until next event
static int    mus_music_volume = 127;   // 0..127, from SetMusicVolume

// Precomputed MIDI-note -> phase increment (cycles per output frame).
// phase_inc = freq / OUT_RATE, freq = 440 * 2^((note-69)/12).
static double mus_note_inc[128];
static int    mus_note_inc_ready = 0;

static void MusicInitNoteTable(void)
{
    int n;

    if (mus_note_inc_ready)
    {
        return;
    }

    for (n = 0; n < 128; ++n)
    {
        double freq = 440.0 * pow(2.0, (n - 69) / 12.0);
        mus_note_inc[n] = freq / (double) OUT_RATE;
    }

    mus_note_inc_ready = 1;
}

// Start (or retrigger) a voice for (channel, note) at the given volume.
static void MusicVoiceOn(int channel, int note, int vol)
{
    int i, free_slot = -1, oldest = 0;

    if (note < 0 || note > 127)
    {
        return;
    }

    // Reuse an existing voice for the same (channel,note) if present, else the
    // first free slot, else steal slot 0 (simple, bounded).
    for (i = 0; i < MAX_VOICES; ++i)
    {
        if (mus_voices[i].active
         && mus_voices[i].channel == channel
         && mus_voices[i].note == note)
        {
            free_slot = i;
            break;
        }
        if (free_slot < 0 && !mus_voices[i].active)
        {
            free_slot = i;
        }
    }

    if (free_slot < 0)
    {
        free_slot = oldest;             // steal (voice cap reached)
    }

    mus_voices[free_slot].active    = 1;
    mus_voices[free_slot].channel   = channel;
    mus_voices[free_slot].note      = note;
    mus_voices[free_slot].vol       = vol;
    mus_voices[free_slot].phase     = 0.0;
    mus_voices[free_slot].phase_inc = mus_note_inc[note];
}

// Stop the voice matching (channel, note), if any.
static void MusicVoiceOff(int channel, int note)
{
    int i;

    for (i = 0; i < MAX_VOICES; ++i)
    {
        if (mus_voices[i].active
         && mus_voices[i].channel == channel
         && mus_voices[i].note == note)
        {
            mus_voices[i].active = 0;
        }
    }
}

static void MusicAllVoicesOff(void)
{
    memset(mus_voices, 0, sizeof(mus_voices));
}

// Read one byte of the score, staying within the owned copy.  Out-of-bounds
// reads end the song (treated like ScoreEnd) rather than reading past the buffer.
static int MusicReadByte(void)
{
    if (mus_current == NULL || mus_pos < 0 || mus_pos >= mus_current->len)
    {
        return -1;
    }
    return mus_current->data[mus_pos++];
}

// Process MUS events starting at mus_pos until one carries the "last" flag,
// then read the trailing variable-length delay (in ticks) and convert it to
// output samples, which we add to mus_tick_samples.  Sets mus_finished/stops or
// loops on ScoreEnd.  Every read is bounds-checked via MusicReadByte().
static void MusicRunEvents(void)
{
    int guard;

    // Bound the work per call so a malformed score can never spin forever:
    // a well-formed group is a handful of events, but cap generously.
    for (guard = 0; guard < 4096; ++guard)
    {
        int event = MusicReadByte();
        int last, type, channel;

        if (event < 0)
        {
            // Ran off the end without a ScoreEnd: stop (or loop).
            goto score_end;
        }

        last    = (event >> 7) & 1;
        type    = (event >> 4) & 7;
        channel = event & 0x0F;

        switch (type)
        {
            case 0:     // ReleaseNote: 1 byte (note)
            {
                int note = MusicReadByte();
                if (note < 0) goto score_end;
                MusicVoiceOff(channel, note & 0x7F);
                break;
            }

            case 1:     // PlayNote: note byte, optional volume byte
            {
                int note = MusicReadByte();
                int vol;
                if (note < 0) goto score_end;

                if (note & 0x80)
                {
                    int v = MusicReadByte();
                    if (v < 0) goto score_end;
                    mus_chan_vol[channel] = v & 0x7F;
                }
                note &= 0x7F;
                vol = mus_chan_vol[channel];

                // Channel 15 is percussion: with a pure-tone synth a "note"
                // there is meaningless, so skip it (silent) to avoid drone.
                if (channel != PERCUSSION_CHAN)
                {
                    MusicVoiceOn(channel, note, vol);
                }
                break;
            }

            case 2:     // PitchBend: 1 byte (ignored — tones stay on-pitch)
            {
                if (MusicReadByte() < 0) goto score_end;
                break;
            }

            case 3:     // SystemEvent: 1 byte (ignored)
            {
                if (MusicReadByte() < 0) goto score_end;
                break;
            }

            case 4:     // Controller: 2 bytes (controller, value)
            {
                int ctrl = MusicReadByte();
                int val;
                if (ctrl < 0) goto score_end;
                val = MusicReadByte();
                if (val < 0) goto score_end;

                // Controller 3 is the MUS "volume" controller; honour it so
                // channel levels track the score.  Others (pan, etc.) ignored.
                if ((ctrl & 0x7F) == 3)
                {
                    mus_chan_vol[channel] = val & 0x7F;
                }
                break;
            }

            case 6:     // ScoreEnd
                goto score_end;

            default:    // 5 and 7 are unused/reserved — treat as end (defensive)
                goto score_end;
        }

        if (last)
        {
            // Variable-length delay (ticks): 7 bits/byte, high bit = continue.
            uint32_t delay = 0;
            int vb, vguard;

            for (vguard = 0; vguard < 5; ++vguard)   // <=5 bytes covers 32 bits
            {
                vb = MusicReadByte();
                if (vb < 0) goto score_end;
                delay = (delay << 7) | (uint32_t)(vb & 0x7F);
                if (!(vb & 0x80))
                {
                    break;
                }
            }

            // Convert ticks -> output samples and accumulate.  Keeping this as
            // a double preserves the fractional 48000/140 ratio across ticks.
            mus_tick_samples += (double) delay * ((double) OUT_RATE / (double) MUS_RATE);
            return;
        }
        // Otherwise loop and read the next event in this same group (delay 0).
    }

    // Fell through the guard (pathological score): stop to stay safe.
score_end:
    if (mus_looping && mus_current != NULL)
    {
        // Restart from the score: reset cursor, voices and channel volumes.
        mus_pos = mus_current->score_start;
        MusicAllVoicesOff();
        // Leave mus_tick_samples as-is (>=0); next call schedules the next group.
        if (mus_tick_samples < 1.0)
        {
            mus_tick_samples += (double) OUT_RATE / (double) MUS_RATE;
        }
    }
    else
    {
        mus_playing  = 0;
        mus_finished = 1;
        MusicAllVoicesOff();
    }
}

// Produce the next mono music sample (already scaled by master music volume),
// advancing the MUS clock by exactly one output frame.  Returns a value in
// roughly int16 range (may be added to the SFX mix; the caller clamps).
static int32_t MusicNextSample(void)
{
    int i;
    int32_t mix;

    if (!mus_playing || mus_paused || mus_current == NULL)
    {
        return 0;
    }

    // Advance the MUS clock by one frame and run due event groups.  A while
    // loop handles zero-delay groups (several events landing on one tick).
    mus_tick_samples -= 1.0;
    {
        int spin = 0;
        while (mus_playing && mus_tick_samples < 1.0 && spin < 1024)
        {
            MusicRunEvents();
            ++spin;
        }
    }

    if (!mus_playing)
    {
        return 0;
    }

    // Sum the active triangle-wave voices.
    //
    // A triangle in [-1,1] from phase p in [0,1):
    //   tri = 4*|p - 0.5| - 1     (peaks +1 at p=0, -1 at p=0.5)
    // Scale each voice by (note vol / 127) and a conservative per-voice
    // amplitude so that several voices summed stay within int16 without harsh
    // clipping.  PER_VOICE_AMP * MAX_VOICES is kept well under 32767.
    {
        const double PER_VOICE_AMP = 1400.0;   // 1400 * 16 = 22400 < 32767
        double acc = 0.0;

        for (i = 0; i < MAX_VOICES; ++i)
        {
            mus_voice_t *v = &mus_voices[i];
            double tri, p;

            if (!v->active)
            {
                continue;
            }

            p = v->phase;
            tri = 4.0 * fabs(p - 0.5) - 1.0;        // -1..+1

            acc += tri * ((double) v->vol / 127.0) * PER_VOICE_AMP;

            p += v->phase_inc;
            if (p >= 1.0)
            {
                p -= (double)(int) p;               // wrap into [0,1)
            }
            v->phase = p;
        }

        // Apply the master music volume (0..127).
        mix = (int32_t)(acc * ((double) mus_music_volume / 127.0));
    }

    return mix;
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

// Mix `nframes` interleaved stereo frames from the active channels into `out`,
// then add the music synth (advancing the MUS clock one step per frame) on top.
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

    // Music pass: add the synth's per-frame sample (mono) to both channels.
    // MusicNextSample() advances the MUS clock one frame per call and returns
    // 0 when not playing, so this runs every chunk regardless of SFX activity
    // and keeps the score in continuous sync with the audio we generate.
    for (i = 0; i < nframes; ++i)
    {
        int32_t m = MusicNextSample();

        if (m != 0)
        {
            out[i * 2 + 0] = clamp_s16((int32_t) out[i * 2 + 0] + m);
            out[i * 2 + 1] = clamp_s16((int32_t) out[i * 2 + 1] + m);
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

// ===========================================================================
// music_module_t implementation (the MUS synth above, exposed to i_sound.c)
// ---------------------------------------------------------------------------
// Actual sound rendering happens in osdev_sound_pump()/MixChunk() via
// MusicNextSample(); these entry points just manage song state.  i_sound.c
// guards every call (music_module != NULL), and our pump drives the clock, so
// Poll is a no-op.
// ===========================================================================

#define MUS_MAGIC0 'M'
#define MUS_MAGIC1 'U'
#define MUS_MAGIC2 'S'
#define MUS_MAGIC3 0x1A

static boolean I_OSDEV_InitMusic(void)
{
    int i;

    MusicInitNoteTable();
    MusicAllVoicesOff();

    for (i = 0; i < MUS_CHANNELS; ++i)
    {
        mus_chan_vol[i] = 127;          // sensible default until the score sets it
    }

    mus_current      = NULL;
    mus_pos          = 0;
    mus_playing      = 0;
    mus_paused       = 0;
    mus_looping      = 0;
    mus_finished     = 0;
    mus_tick_samples = 0.0;

    return true;
}

static void I_OSDEV_ShutdownMusic(void)
{
    mus_playing  = 0;
    mus_current  = NULL;
    MusicAllVoicesOff();
}

static void I_OSDEV_SetMusicVolume(int volume)
{
    if (volume < 0)   volume = 0;
    if (volume > 127) volume = 127;
    mus_music_volume = volume;
}

static void I_OSDEV_PauseMusic(void)
{
    mus_paused = 1;
}

static void I_OSDEV_ResumeMusic(void)
{
    mus_paused = 0;
}

// Validate the MUS header and take a private copy of the lump.  Returns an
// opaque handle (mus_song_t*) or NULL if the lump is not valid MUS.
static void *I_OSDEV_RegisterSong(void *data, int len)
{
    const uint8_t *src = (const uint8_t *) data;
    mus_song_t *song;
    int score_len, score_start;

    // Need at least the fixed 16-byte MUS header.
    if (src == NULL || len < 16)
    {
        return NULL;
    }

    // Magic "MUS\x1A".
    if (src[0] != MUS_MAGIC0 || src[1] != MUS_MAGIC1
     || src[2] != MUS_MAGIC2 || src[3] != MUS_MAGIC3)
    {
        return NULL;
    }

    // u16 scoreLen, u16 scoreStart (little-endian) at offsets 4 and 6.
    score_len   = (int) (src[4] | (src[5] << 8));
    score_start = (int) (src[6] | (src[7] << 8));

    // Bounds: the score must lie within the lump.  Clamp score_len to what the
    // lump actually holds so every later read stays in range even if the
    // header lies or the lump is truncated.
    if (score_start < 0 || score_start > len)
    {
        return NULL;
    }
    if (score_len < 0 || score_start + score_len > len)
    {
        score_len = len - score_start;
    }

    song = malloc(sizeof(mus_song_t));
    if (song == NULL)
    {
        return NULL;
    }

    song->data = malloc((size_t) len);
    if (song->data == NULL)
    {
        free(song);
        return NULL;
    }

    memcpy(song->data, src, (size_t) len);
    song->len         = len;
    song->score_start = score_start;
    song->score_len   = score_len;

    return song;
}

static void I_OSDEV_UnRegisterSong(void *handle)
{
    mus_song_t *song = (mus_song_t *) handle;

    if (song == NULL)
    {
        return;
    }

    // If we're freeing the song that's currently selected, stop first.
    if (song == mus_current)
    {
        mus_playing = 0;
        mus_current = NULL;
        MusicAllVoicesOff();
    }

    free(song->data);
    free(song);
}

static void I_OSDEV_PlaySong(void *handle, boolean looping)
{
    mus_song_t *song = (mus_song_t *) handle;
    int i;

    if (song == NULL)
    {
        return;
    }

    MusicInitNoteTable();
    MusicAllVoicesOff();

    for (i = 0; i < MUS_CHANNELS; ++i)
    {
        mus_chan_vol[i] = 127;
    }

    mus_current      = song;
    mus_pos          = song->score_start;
    mus_looping      = looping ? 1 : 0;
    mus_finished     = 0;
    mus_paused       = 0;
    mus_tick_samples = 0.0;             // first event group runs immediately
    mus_playing      = 1;
}

static void I_OSDEV_StopSong(void)
{
    mus_playing = 0;
    MusicAllVoicesOff();
}

static boolean I_OSDEV_MusicIsPlaying(void)
{
    return (mus_playing && !mus_finished) ? true : false;
}

static void I_OSDEV_PollMusic(void)
{
    // Rendering + the MUS clock are driven from osdev_sound_pump(); nothing to do.
}

// snd_musicdevice defaults to SNDDEVICE_SB (see i_sound.c).  Music module
// selection in i_sound.c's InitMusicModule() assigns DG_music_module directly
// (no device-list scan), so this list is informational; claim SB for parity.
static snddevice_t music_osdev_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_GUS,
    SNDDEVICE_AWE32,
};

music_module_t DG_music_module =
{
    music_osdev_devices,
    sizeof(music_osdev_devices) / sizeof(*music_osdev_devices),
    I_OSDEV_InitMusic,
    I_OSDEV_ShutdownMusic,
    I_OSDEV_SetMusicVolume,
    I_OSDEV_PauseMusic,
    I_OSDEV_ResumeMusic,
    I_OSDEV_RegisterSong,
    I_OSDEV_UnRegisterSong,
    I_OSDEV_PlaySong,
    I_OSDEV_StopSong,
    I_OSDEV_MusicIsPlaying,
    I_OSDEV_PollMusic,
};
