#include "doomtype.h"
#include "i_sound.h"
#include "memio.h"
#include "mus2mid.h"
#include "w_wad.h"
#include "z_zone.h"

#include <bleed_audio.h>
#include <stdio.h>
#include <string.h>

#define SFX_CHANNELS    16
#define DMX_HEADER      8
#define DMX_PAD         16

int   use_libsamplerate   = 0;
float libsamplerate_scale = 0.65f;

static boolean use_sfx_prefix;
static int     channel_voice[SFX_CHANNELS];

static snddevice_t bleed_devices[] =
{
    SNDDEVICE_ADLIB,
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

// sound effects

static au_sample_t *sfx_sample(sfxinfo_t *sfx)
{
    if (sfx->driver_data)
        return sfx->driver_data;

    int   len  = W_LumpLength(sfx->lumpnum);
    byte *data = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);

    if (len < DMX_HEADER || data[0] != 0x03 || data[1] != 0x00)
        return NULL;

    uint32_t rate   = data[2] | (data[3] << 8);
    uint32_t length = data[4] | (data[5] << 8) | (data[6] << 16) | ((uint32_t)data[7] << 24);
    if (length > (uint32_t)len - DMX_HEADER || length <= DMX_PAD * 2 + 16 || !rate)
        return NULL;

    au_sample_t *sample = Z_Malloc(sizeof(au_sample_t), PU_STATIC, 0);
    sample->data     = data + DMX_HEADER + DMX_PAD;
    sample->frames   = length - DMX_PAD * 2;
    sample->rate     = rate;
    sample->format   = AU_U8;
    sample->channels = 1;

    sfx->driver_data = sample;
    return sample;
}

static int Bleed_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char name[9];

    if (sfx->link)
        sfx = sfx->link;

    if (use_sfx_prefix)
        snprintf(name, sizeof(name), "ds%s", sfx->name);
    else
        snprintf(name, sizeof(name), "%s", sfx->name);

    return W_GetNumForName(name);
}

static boolean Bleed_InitSound(boolean prefix)
{
    use_sfx_prefix = prefix;
    for (int i = 0; i < SFX_CHANNELS; i++)
        channel_voice[i] = -1;

    int r = au_open();
    if (r < 0) {
        printf("I_InitSound: no sound card (%d), playing silent\n", r);
        return false;
    }
    return true;
}

static void Bleed_ShutdownSound(void)
{
    au_close();
}

// libaudio's worker keeps the card fed, we dont need to do anything here bleed handles it
static void Bleed_UpdateSound(void)
{
}

static void Bleed_UpdateSoundParams(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= SFX_CHANNELS)
        return;
    au_voice_set(channel_voice[channel], vol, sep / 2);
}

static int Bleed_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= SFX_CHANNELS)
        return -1;

    au_voice_stop(channel_voice[channel]);
    channel_voice[channel] = -1;

    au_sample_t *sample = sfx_sample(sfx);
    if (!sample)
        return -1;

    channel_voice[channel] = au_play(sample, vol, sep / 2, 0);
    return channel_voice[channel] < 0 ? -1 : channel;
}

static void Bleed_StopSound(int channel)
{
    if (channel < 0 || channel >= SFX_CHANNELS)
        return;
    au_voice_stop(channel_voice[channel]);
    channel_voice[channel] = -1;
}

static boolean Bleed_SoundIsPlaying(int channel)
{
    if (channel < 0 || channel >= SFX_CHANNELS)
        return false;
    return au_voice_playing(channel_voice[channel]);
}

sound_module_t DG_sound_module =
{
    bleed_devices,
    arrlen(bleed_devices),
    Bleed_InitSound,
    Bleed_ShutdownSound,
    Bleed_GetSfxLumpNum,
    Bleed_UpdateSound,
    Bleed_UpdateSoundParams,
    Bleed_StartSound,
    Bleed_StopSound,
    Bleed_SoundIsPlaying,
    NULL,
};

// music

static boolean Bleed_InitMusic(void)
{
    if (au_open() < 0)
        return false;

    if (au_bank_load_default() < 0) {
        printf("I_InitMusic: no soundfont in " AU_SOUNDFONT_DIR ", no music\n");
        return false;
    }
    return true;
}

static void Bleed_ShutdownMusic(void)
{
    au_song_stop();
}

static void Bleed_SetMusicVolume(int volume)
{
    au_music_volume(volume);
}

static void Bleed_PauseMusic(void)
{
    au_song_pause(1);
}

static void Bleed_ResumeMusic(void)
{
    au_song_pause(0);
}

static void *Bleed_RegisterSong(void *data, int len)
{
    if (len > 4 && !memcmp(data, "MThd", 4))
        return au_song_load(data, len);

    MEMFILE *in  = mem_fopen_read(data, len);
    MEMFILE *out = mem_fopen_write();
    au_song_t *song = NULL;

    if (mus2mid(in, out) == 0) {
        void  *mid;
        size_t mid_len;
        mem_get_buf(out, &mid, &mid_len);
        song = au_song_load(mid, mid_len);
    }

    mem_fclose(in);
    mem_fclose(out);
    return song;
}

static void Bleed_UnRegisterSong(void *handle)
{
    au_song_free(handle);
}

static void Bleed_PlaySong(void *handle, boolean looping)
{
    au_song_play(handle, looping);
}

static void Bleed_StopSong(void)
{
    au_song_stop();
}

static boolean Bleed_MusicIsPlaying(void)
{
    return au_song_playing();
}

music_module_t DG_music_module =
{
    bleed_devices,
    arrlen(bleed_devices),
    Bleed_InitMusic,
    Bleed_ShutdownMusic,
    Bleed_SetMusicVolume,
    Bleed_PauseMusic,
    Bleed_ResumeMusic,
    Bleed_RegisterSong,
    Bleed_UnRegisterSong,
    Bleed_PlaySong,
    Bleed_StopSong,
    Bleed_MusicIsPlaying,
    NULL,
};
