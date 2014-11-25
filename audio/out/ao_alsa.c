/*
 * ALSA 0.9.x-1.x audio output driver
 *
 * Copyright (C) 2004 Alex Beregszaszi
 * Zsolt Barat <joy@streamminister.de>
 *
 * modified for real ALSA 0.9.0 support by Zsolt Barat <joy@streamminister.de>
 * additional AC-3 passthrough support by Andy Lo A Foe <andy@alsaplayer.org>
 * 08/22/2002 iec958-init rewritten and merged with common init, zsolt
 * 04/13/2004 merged with ao_alsa1.x, fixes provided by Jindrich Makovicka
 * 04/25/2004 printfs converted to mp_msg, Zsolt.
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>

#include "config.h"
#include "options/options.h"
#include "options/m_option.h"
#include "common/msg.h"
#include "osdep/endian.h"

#include <alsa/asoundlib.h>

#define HAVE_CHMAP_API \
    (defined(SND_CHMAP_API_VERSION) && SND_CHMAP_API_VERSION >= (1 << 16))

#include "ao.h"
#include "internal.h"
#include "audio/format.h"

struct priv {
    snd_pcm_t *alsa;
    snd_pcm_format_t alsa_fmt;
    int can_pause;
    snd_pcm_sframes_t prepause_frames;
    double delay_before_pause;
    int buffersize; // in frames
    int outburst; // in frames

    int cfg_block;
    char *cfg_device;
    char *cfg_mixer_device;
    char *cfg_mixer_name;
    int cfg_mixer_index;
    int cfg_resample;
    int cfg_ni;
};

#define BUFFER_TIME 250000  // 250ms
#define FRAGCOUNT 16

#define CHECK_ALSA_ERROR(message) \
    do { \
        if (err < 0) { \
            MP_ERR(ao, "%s: %s\n", (message), snd_strerror(err)); \
            goto alsa_error; \
        } \
    } while (0)

#define CHECK_ALSA_WARN(message) \
    do { \
        if (err < 0) \
            MP_WARN(ao, "%s: %s\n", (message), snd_strerror(err)); \
    } while (0)

static int control(struct ao *ao, enum aocontrol cmd, void *arg)
{
    struct priv *p = ao->priv;
    snd_mixer_t *handle = NULL;
    switch (cmd) {
    case AOCONTROL_GET_MUTE:
    case AOCONTROL_SET_MUTE:
    case AOCONTROL_GET_VOLUME:
    case AOCONTROL_SET_VOLUME:
    {
        int err;
        snd_mixer_elem_t *elem;
        snd_mixer_selem_id_t *sid;

        long pmin, pmax;
        long get_vol, set_vol;
        float f_multi;

        if (AF_FORMAT_IS_SPECIAL(ao->format))
            return CONTROL_FALSE;

        snd_mixer_selem_id_alloca(&sid);

        snd_mixer_selem_id_set_index(sid, p->cfg_mixer_index);
        snd_mixer_selem_id_set_name(sid, p->cfg_mixer_name);

        err = snd_mixer_open(&handle, 0);
        CHECK_ALSA_ERROR("Mixer open error");

        err = snd_mixer_attach(handle, p->cfg_mixer_device);
        CHECK_ALSA_ERROR("Mixer attach error");

        err = snd_mixer_selem_register(handle, NULL, NULL);
        CHECK_ALSA_ERROR("Mixer register error");

        err = snd_mixer_load(handle);
        CHECK_ALSA_ERROR("Mixer load error");

        elem = snd_mixer_find_selem(handle, sid);
        if (!elem) {
            MP_VERBOSE(ao, "Unable to find simple control '%s',%i.\n",
                       snd_mixer_selem_id_get_name(sid),
                       snd_mixer_selem_id_get_index(sid));
            goto alsa_error;
        }

        snd_mixer_selem_get_playback_volume_range(elem, &pmin, &pmax);
        f_multi = (100 / (float)(pmax - pmin));

        switch (cmd) {
        case AOCONTROL_SET_VOLUME: {
            ao_control_vol_t *vol = arg;
            set_vol = vol->left / f_multi + pmin + 0.5;

            err = snd_mixer_selem_set_playback_volume
                    (elem, SND_MIXER_SCHN_FRONT_LEFT, set_vol);
            CHECK_ALSA_ERROR("Error setting left channel");
            MP_DBG(ao, "left=%li, ", set_vol);

            set_vol = vol->right / f_multi + pmin + 0.5;

            err = snd_mixer_selem_set_playback_volume
                    (elem, SND_MIXER_SCHN_FRONT_RIGHT, set_vol);
            CHECK_ALSA_ERROR("Error setting right channel");
            MP_DBG(ao, "right=%li, pmin=%li, pmax=%li, mult=%f\n",
                   set_vol, pmin, pmax, f_multi);
            break;
        }
        case AOCONTROL_GET_VOLUME: {
            ao_control_vol_t *vol = arg;
            snd_mixer_selem_get_playback_volume
                (elem, SND_MIXER_SCHN_FRONT_LEFT, &get_vol);
            vol->left = (get_vol - pmin) * f_multi;
            snd_mixer_selem_get_playback_volume
                (elem, SND_MIXER_SCHN_FRONT_RIGHT, &get_vol);
            vol->right = (get_vol - pmin) * f_multi;
            MP_DBG(ao, "left=%f, right=%f\n", vol->left, vol->right);
            break;
        }
        case AOCONTROL_SET_MUTE: {
            bool *mute = arg;
            if (!snd_mixer_selem_has_playback_switch(elem))
                goto alsa_error;
            if (!snd_mixer_selem_has_playback_switch_joined(elem)) {
                snd_mixer_selem_set_playback_switch
                    (elem, SND_MIXER_SCHN_FRONT_RIGHT, !*mute);
            }
            snd_mixer_selem_set_playback_switch
                (elem, SND_MIXER_SCHN_FRONT_LEFT, !*mute);
            break;
        }
        case AOCONTROL_GET_MUTE: {
            bool *mute = arg;
            if (!snd_mixer_selem_has_playback_switch(elem))
                goto alsa_error;
            int tmp = 1;
            snd_mixer_selem_get_playback_switch
                (elem, SND_MIXER_SCHN_FRONT_LEFT, &tmp);
            *mute = !tmp;
            if (!snd_mixer_selem_has_playback_switch_joined(elem)) {
                snd_mixer_selem_get_playback_switch
                    (elem, SND_MIXER_SCHN_FRONT_RIGHT, &tmp);
                *mute &= !tmp;
            }
            break;
        }
        }
        snd_mixer_close(handle);
        return CONTROL_OK;
    }

    } //end switch
    return CONTROL_UNKNOWN;

alsa_error:
    if (handle)
        snd_mixer_close(handle);
    return CONTROL_ERROR;
}

static const int mp_to_alsa_format[][2] = {
    {AF_FORMAT_S8,          SND_PCM_FORMAT_S8},
    {AF_FORMAT_U8,          SND_PCM_FORMAT_U8},
    {AF_FORMAT_U16,         SND_PCM_FORMAT_U16},
    {AF_FORMAT_S16,         SND_PCM_FORMAT_S16},
    {AF_FORMAT_U32,         SND_PCM_FORMAT_U32},
    {AF_FORMAT_S32,         SND_PCM_FORMAT_S32},
    {AF_FORMAT_U24,
            MP_SELECT_LE_BE(SND_PCM_FORMAT_U24_3LE, SND_PCM_FORMAT_U24_3BE)},
    {AF_FORMAT_S24,
            MP_SELECT_LE_BE(SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S24_3BE)},
    {AF_FORMAT_FLOAT,       SND_PCM_FORMAT_FLOAT},
    {AF_FORMAT_UNKNOWN,     SND_PCM_FORMAT_UNKNOWN},
};

static int find_alsa_format(int af_format)
{
    af_format = af_fmt_from_planar(af_format);
    for (int n = 0; mp_to_alsa_format[n][0] != AF_FORMAT_UNKNOWN; n++) {
        if (mp_to_alsa_format[n][0] == af_format)
            return mp_to_alsa_format[n][1];
    }
    return SND_PCM_FORMAT_UNKNOWN;
}

#if HAVE_CHMAP_API

static const int alsa_to_mp_channels[][2] = {
    {SND_CHMAP_FL,      MP_SP(FL)},
    {SND_CHMAP_FR,      MP_SP(FR)},
    {SND_CHMAP_RL,      MP_SP(BL)},
    {SND_CHMAP_RR,      MP_SP(BR)},
    {SND_CHMAP_FC,      MP_SP(FC)},
    {SND_CHMAP_LFE,     MP_SP(LFE)},
    {SND_CHMAP_SL,      MP_SP(SL)},
    {SND_CHMAP_SR,      MP_SP(SR)},
    {SND_CHMAP_RC,      MP_SP(BC)},
    {SND_CHMAP_FLC,     MP_SP(FLC)},
    {SND_CHMAP_FRC,     MP_SP(FRC)},
    {SND_CHMAP_FLW,     MP_SP(WL)},
    {SND_CHMAP_FRW,     MP_SP(WR)},
    {SND_CHMAP_TC,      MP_SP(TC)},
    {SND_CHMAP_TFL,     MP_SP(TFL)},
    {SND_CHMAP_TFR,     MP_SP(TFR)},
    {SND_CHMAP_TFC,     MP_SP(TFC)},
    {SND_CHMAP_TRL,     MP_SP(TBL)},
    {SND_CHMAP_TRR,     MP_SP(TBR)},
    {SND_CHMAP_TRC,     MP_SP(TBC)},
    {SND_CHMAP_MONO,    MP_SP(FC)},
    {SND_CHMAP_LAST,    MP_SPEAKER_ID_COUNT}
};

static int find_mp_channel(int alsa_channel)
{
    for (int i = 0; alsa_to_mp_channels[i][1] != MP_SPEAKER_ID_COUNT; i++) {
        if (alsa_to_mp_channels[i][0] == alsa_channel)
            return alsa_to_mp_channels[i][1];
    }

    return MP_SPEAKER_ID_COUNT;
}

static int find_alsa_channel(int mp_channel)
{
    for (int i = 0; alsa_to_mp_channels[i][1] != MP_SPEAKER_ID_COUNT; i++) {
        if (alsa_to_mp_channels[i][1] == mp_channel)
            return alsa_to_mp_channels[i][0];
    }

    return SND_CHMAP_UNKNOWN;
}

static bool query_chmaps(struct ao *ao, struct mp_chmap *chmap)
{
    struct priv *p = ao->priv;
    struct mp_chmap_sel chmap_sel = {0};

    snd_pcm_chmap_query_t **maps = snd_pcm_query_chmaps(p->alsa);
    if (!maps)
        return false;

    for (int i = 0; maps[i] != NULL; i++) {
        if (maps[i]->map.channels > MP_NUM_CHANNELS) {
            MP_VERBOSE(ao, "skipping ALSA channel map with too many channels.\n");
            continue;
        }

        struct mp_chmap entry = {.num = maps[i]->map.channels};
        for (int c = 0; c < entry.num; c++)
            entry.speaker[c] = find_mp_channel(maps[i]->map.pos[c]);

        if (mp_chmap_is_valid(&entry)) {
            MP_VERBOSE(ao, "Got supported channel map: %s (type %s)\n",
                       mp_chmap_to_str(&entry),
                       snd_pcm_chmap_type_name(maps[i]->type));
            mp_chmap_sel_add_map(&chmap_sel, &entry);
        } else {
            char tmp[128];
            if (snd_pcm_chmap_print(&maps[i]->map, sizeof(tmp), tmp) > 0)
                MP_VERBOSE(ao, "skipping unknown ALSA channel map: %s\n", tmp);
        }
    }

    snd_pcm_free_chmaps(maps);

    return ao_chmap_sel_adjust(ao, &chmap_sel, chmap);
}

#else /* HAVE_CHMAP_API */

static bool query_chmaps(struct ao *ao, struct mp_chmap *chmap)
{
    return false;
}

#endif /* else HAVE_CHMAP_API */

// Lists device names and their implied channel map.
// The second item must be resolvable with mp_chmap_from_str().
// Source: http://www.alsa-project.org/main/index.php/DeviceNames
// (Speaker names are slightly different from mpv's.)
static const char *const device_channel_layouts[][2] = {
    {"default",         "fc"},
    {"default",         "fl-fr"},
    {"rear",            "bl-br"},
    {"center_lfe",      "fc-lfe"},
    {"side",            "sl-sr"},
    {"surround40",      "fl-fr-bl-br"},
    {"surround50",      "fl-fr-bl-br-fc"},
    {"surround41",      "fl-fr-bl-br-lfe"},
    {"surround51",      "fl-fr-bl-br-fc-lfe"},
    {"surround71",      "fl-fr-bl-br-fc-lfe-sl-sr"},
};

#define NUM_ALSA_CHMAPS MP_ARRAY_SIZE(device_channel_layouts)

static const char *select_chmap(struct ao *ao, struct mp_chmap *chmap)
{
    struct mp_chmap_sel sel = {0};
    struct mp_chmap maps[NUM_ALSA_CHMAPS];
    for (int n = 0; n < NUM_ALSA_CHMAPS; n++) {
        mp_chmap_from_str(&maps[n], bstr0(device_channel_layouts[n][1]));
        mp_chmap_sel_add_map(&sel, &maps[n]);
    };

    if (!ao_chmap_sel_adjust(ao, &sel, chmap))
        return "default";

    for (int n = 0; n < NUM_ALSA_CHMAPS; n++) {
        if (mp_chmap_equals(chmap, &maps[n]))
            return device_channel_layouts[n][0];
    }

    MP_ERR(ao, "channel layout %s (%d ch) not supported.\n",
           mp_chmap_to_str(chmap), chmap->num);
    return "default";
}

static int map_iec958_srate(int srate)
{
    switch (srate) {
    case 44100:     return IEC958_AES3_CON_FS_44100;
    case 48000:     return IEC958_AES3_CON_FS_48000;
    case 32000:     return IEC958_AES3_CON_FS_32000;
    case 22050:     return IEC958_AES3_CON_FS_22050;
    case 24000:     return IEC958_AES3_CON_FS_24000;
    case 88200:     return IEC958_AES3_CON_FS_88200;
    case 768000:    return IEC958_AES3_CON_FS_768000;
    case 96000:     return IEC958_AES3_CON_FS_96000;
    case 176400:    return IEC958_AES3_CON_FS_176400;
    case 192000:    return IEC958_AES3_CON_FS_192000;
    default:        return IEC958_AES3_CON_FS_NOTID;
    }
}

// ALSA device strings can have parameters. They are usually appended to the
// device name. Since there can be various forms, and we (sometimes) want to
// append them to unknown device strings, which possibly already include params.
static char *append_params(void *ta_parent, const char *device, const char *p)
{
    if (!p || !p[0])
        return talloc_strdup(ta_parent, device);

    int len = strlen(device);
    char *end = strchr(device, ':');
    if (!end) {
        /* no existing parameters: add it behind device name */
        return talloc_asprintf(ta_parent, "%s:%s", device, p);
    } else if (end[1] == '\0') {
        /* ":" but no parameters */
        return talloc_asprintf(ta_parent, "%s%s", device, p);
    } else if (end[1] == '{' && device[len - 1] == '}') {
        /* parameters in config syntax: add it inside the { } block */
        return talloc_asprintf(ta_parent, "%.*s %s}", len - 1, device, p);
    } else {
        /* a simple list of parameters: add it at the end of the list */
        return talloc_asprintf(ta_parent, "%s,%s", device, p);
    }
    abort();
}

static int try_open_device(struct ao *ao, const char *device, int open_mode)
{
    struct priv *p = ao->priv;

    if (AF_FORMAT_IS_IEC61937(ao->format)) {
        void *tmp = talloc_new(NULL);
        char *params = talloc_asprintf(tmp,
                        "AES0=%d,AES1=%d,AES2=0,AES3=%d",
                        IEC958_AES0_NONAUDIO | IEC958_AES0_PRO_EMPHASIS_NONE,
                        IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
                        map_iec958_srate(ao->samplerate));
        const char *ac3_device = append_params(tmp, device, params);
        int err = snd_pcm_open
                    (&p->alsa, ac3_device, SND_PCM_STREAM_PLAYBACK, open_mode);
        talloc_free(tmp);
        if (!err)
            return 0;
    }

    return snd_pcm_open(&p->alsa, device, SND_PCM_STREAM_PLAYBACK, open_mode);
}

static void uninit(struct ao *ao)
{
    struct priv *p = ao->priv;

    if (p->alsa) {
        int err;

        err = snd_pcm_close(p->alsa);
        CHECK_ALSA_ERROR("pcm close error");
    }

alsa_error: ;
}

static int init(struct ao *ao)
{
    struct priv *p = ao->priv;
    int err;

    if (!p->cfg_ni)
        ao->format = af_fmt_from_planar(ao->format);

    struct mp_chmap implied_chmap = ao->channels;
    const char *device;
    if (AF_FORMAT_IS_IEC61937(ao->format)) {
        device = "iec958";
        MP_VERBOSE(ao, "playing AC3/iec61937/iec958, %i channels\n",
                   ao->channels.num);
    } else {
        device = select_chmap(ao, &implied_chmap);
        // Not-default likely means a hw device - enable software conversions.
        if (strcmp(device, "default") != 0)
            device = talloc_asprintf(ao, "plug:%s", device);
    }
    if (ao->device)
        device = ao->device;
    if (p->cfg_device && p->cfg_device[0])
        device = p->cfg_device;

    MP_VERBOSE(ao, "using device: %s\n", device);
    MP_VERBOSE(ao, "using ALSA version: %s\n", snd_asoundlib_version());

    err = try_open_device(ao, device, p->cfg_block ? 0 : SND_PCM_NONBLOCK);
    if (err < 0) {
        if (err != -EBUSY && !p->cfg_block) {
            MP_WARN(ao, "Open in nonblock-mode "
                    "failed, trying to open in block-mode.\n");
            err = try_open_device(ao, device, 0);
        }
        CHECK_ALSA_ERROR("Playback open error");
    }

    err = snd_pcm_nonblock(p->alsa, 0);
    if (err < 0) {
        MP_ERR(ao, "Error setting block-mode: %s.\n", snd_strerror(err));
    } else {
        MP_VERBOSE(ao, "pcm opened in blocking mode\n");
    }

    snd_pcm_hw_params_t *alsa_hwparams;
    snd_pcm_sw_params_t *alsa_swparams;

    snd_pcm_hw_params_alloca(&alsa_hwparams);
    snd_pcm_sw_params_alloca(&alsa_swparams);

    err = snd_pcm_hw_params_any(p->alsa, alsa_hwparams);
    CHECK_ALSA_ERROR("Unable to get initial parameters");

    if (AF_FORMAT_IS_IEC61937(ao->format)) {
        if (ao->format == AF_FORMAT_S_MP3) {
            p->alsa_fmt = SND_PCM_FORMAT_MPEG;
        } else {
            p->alsa_fmt = SND_PCM_FORMAT_S16;
        }
    } else {
        p->alsa_fmt = find_alsa_format(ao->format);
    }
    if (p->alsa_fmt == SND_PCM_FORMAT_UNKNOWN) {
        p->alsa_fmt = SND_PCM_FORMAT_S16;
        ao->format = AF_FORMAT_S16;
    }

    err = snd_pcm_hw_params_test_format(p->alsa, alsa_hwparams, p->alsa_fmt);
    if (err < 0) {
        if (AF_FORMAT_IS_IEC61937(ao->format))
            CHECK_ALSA_ERROR("Unable to set IEC61937 format");
        MP_INFO(ao, "Format %s is not supported by hardware, trying default.\n",
                af_fmt_to_str(ao->format));
        p->alsa_fmt = SND_PCM_FORMAT_S16;
        ao->format = AF_FORMAT_S16;
    }

    err = snd_pcm_hw_params_set_format(p->alsa, alsa_hwparams, p->alsa_fmt);
    CHECK_ALSA_ERROR("Unable to set format");

    snd_pcm_access_t access = af_fmt_is_planar(ao->format)
                                    ? SND_PCM_ACCESS_RW_NONINTERLEAVED
                                    : SND_PCM_ACCESS_RW_INTERLEAVED;
    err = snd_pcm_hw_params_set_access(p->alsa, alsa_hwparams, access);
    if (err < 0 && af_fmt_is_planar(ao->format)) {
        ao->format = af_fmt_from_planar(ao->format);
        access = SND_PCM_ACCESS_RW_INTERLEAVED;
        err = snd_pcm_hw_params_set_access(p->alsa, alsa_hwparams, access);
    }
    CHECK_ALSA_ERROR("Unable to set access type");

    struct mp_chmap dev_chmap = ao->channels;
    if (query_chmaps(ao, &dev_chmap)) {
        ao->channels = dev_chmap;
    } else {
        dev_chmap.num = 0;
        ao->channels = implied_chmap;
    }

    int num_channels = ao->channels.num;
    err = snd_pcm_hw_params_set_channels_near
            (p->alsa, alsa_hwparams, &num_channels);
    CHECK_ALSA_ERROR("Unable to set channels");

    if (num_channels > MP_NUM_CHANNELS) {
        MP_FATAL(ao, "Too many audio channels (%d).\n", num_channels);
        goto alsa_error;
    }

    if (num_channels != ao->channels.num) {
        MP_ERR(ao, "Couldn't get requested number of channels.\n");
        mp_chmap_from_channels_alsa(&ao->channels, num_channels);
    }

    // Some ALSA drivers have broken delay reporting, so disable the ALSA
    // resampling plugin by default.
    if (!p->cfg_resample) {
        err = snd_pcm_hw_params_set_rate_resample(p->alsa, alsa_hwparams, 0);
        CHECK_ALSA_ERROR("Unable to disable resampling");
    }

    err = snd_pcm_hw_params_set_rate_near
            (p->alsa, alsa_hwparams, &ao->samplerate, NULL);
    CHECK_ALSA_ERROR("Unable to set samplerate-2");

    err = snd_pcm_hw_params_set_buffer_time_near
            (p->alsa, alsa_hwparams, &(unsigned int){BUFFER_TIME}, NULL);
    CHECK_ALSA_WARN("Unable to set buffer time near");

    err = snd_pcm_hw_params_set_periods_near
            (p->alsa, alsa_hwparams, &(unsigned int){FRAGCOUNT}, NULL);
    CHECK_ALSA_WARN("Unable to set periods");

    /* finally install hardware parameters */
    err = snd_pcm_hw_params(p->alsa, alsa_hwparams);
    CHECK_ALSA_ERROR("Unable to set hw-parameters");

    /* end setting hw-params */

#if HAVE_CHMAP_API
    if (mp_chmap_is_valid(&dev_chmap)) {
        snd_pcm_chmap_t *alsa_chmap =
            calloc(1, sizeof(*alsa_chmap) +
                      sizeof(alsa_chmap->pos[0]) * dev_chmap.num);
        if (!alsa_chmap)
            goto alsa_error;

        alsa_chmap->channels = dev_chmap.num;
        for (int c = 0; c < dev_chmap.num; c++)
            alsa_chmap->pos[c] = find_alsa_channel(dev_chmap.speaker[c]);

        char tmp[128];
        if (snd_pcm_chmap_print(alsa_chmap, sizeof(tmp), tmp) > 0)
            MP_VERBOSE(ao, "trying to set ALSA channel map: %s\n", tmp);

        err = snd_pcm_set_chmap(p->alsa, alsa_chmap);
        if (err == -ENXIO) {
            MP_WARN(ao, "Device does not support requested channel map\n");
        } else {
            CHECK_ALSA_WARN("Channel map setup failed");
        }
    }

    snd_pcm_chmap_t *alsa_chmap = snd_pcm_get_chmap(p->alsa);
    if (alsa_chmap) {
        char tmp[128];
        if (snd_pcm_chmap_print(alsa_chmap, sizeof(tmp), tmp) > 0)
            MP_VERBOSE(ao, "channel map reported by ALSA: %s\n", tmp);

        struct mp_chmap chmap = {.num = alsa_chmap->channels};
        for (int c = 0; c < chmap.num; c++)
            chmap.speaker[c] = find_mp_channel(alsa_chmap->pos[c]);

        MP_VERBOSE(ao, "which we understand as: %s\n", mp_chmap_to_str(&chmap));

        if (mp_chmap_is_valid(&chmap)) {
            if (mp_chmap_equals(&chmap, &ao->channels)) {
                MP_VERBOSE(ao, "which is what we requested.\n");
            } else if (chmap.num == ao->channels.num) {
                MP_VERBOSE(ao, "using the ALSA channel map.\n");
                ao->channels = chmap;
            } else {
                MP_WARN(ao, "ALSA channel map conflicts with channel count!\n");
            }
        } else {
            MP_WARN(ao, "Got unknown channel map from ALSA.\n");
        }

        free(alsa_chmap);
    }
#endif

    snd_pcm_uframes_t bufsize;
    err = snd_pcm_hw_params_get_buffer_size(alsa_hwparams, &bufsize);
    CHECK_ALSA_ERROR("Unable to get buffersize");

    p->buffersize = bufsize;
    MP_VERBOSE(ao, "got buffersize=%i samples\n", p->buffersize);

    snd_pcm_uframes_t chunk_size;
    err = snd_pcm_hw_params_get_period_size(alsa_hwparams, &chunk_size, NULL);
    CHECK_ALSA_ERROR("Unable to get period size");

    MP_VERBOSE(ao, "got period size %li\n", chunk_size);
    p->outburst = chunk_size;

    /* setting software parameters */
    err = snd_pcm_sw_params_current(p->alsa, alsa_swparams);
    CHECK_ALSA_ERROR("Unable to get sw-parameters");

    snd_pcm_uframes_t boundary;
    err = snd_pcm_sw_params_get_boundary(alsa_swparams, &boundary);
    CHECK_ALSA_ERROR("Unable to get boundary");

    /* start playing when one period has been written */
    err = snd_pcm_sw_params_set_start_threshold
            (p->alsa, alsa_swparams, chunk_size);
    CHECK_ALSA_ERROR("Unable to set start threshold");

    /* disable underrun reporting */
    err = snd_pcm_sw_params_set_stop_threshold
            (p->alsa, alsa_swparams, boundary);
    CHECK_ALSA_ERROR("Unable to set stop threshold");

    /* play silence when there is an underrun */
    err = snd_pcm_sw_params_set_silence_size
            (p->alsa, alsa_swparams, boundary);
    CHECK_ALSA_ERROR("Unable to set silence size");

    err = snd_pcm_sw_params(p->alsa, alsa_swparams);
    CHECK_ALSA_ERROR("Unable to set sw-parameters");

    /* end setting sw-params */

    p->can_pause = snd_pcm_hw_params_can_pause(alsa_hwparams);

    return 0;

alsa_error:
    uninit(ao);
    return -1;
}

static void drain(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_drain(p->alsa);
}

static int get_space(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_status_t *status;
    int err;

    snd_pcm_status_alloca(&status);

    err = snd_pcm_status(p->alsa, status);
    CHECK_ALSA_ERROR("cannot get pcm status");

    unsigned space = snd_pcm_status_get_avail(status);
    if (space > p->buffersize) // Buffer underrun?
        space = p->buffersize;
    return space / p->outburst * p->outburst;

alsa_error:
    return 0;
}

/* delay in seconds between first and last sample in buffer */
static double get_delay(struct ao *ao)
{
    struct priv *p = ao->priv;
    snd_pcm_sframes_t delay;

    if (snd_pcm_state(p->alsa) == SND_PCM_STATE_PAUSED)
        return p->delay_before_pause;

    if (snd_pcm_delay(p->alsa, &delay) < 0)
        return 0;

    if (delay < 0) {
        /* underrun - move the application pointer forward to catch up */
        snd_pcm_forward(p->alsa, -delay);
        delay = 0;
    }
    return delay / (double)ao->samplerate;
}

static void audio_pause(struct ao *ao)
{
    struct priv *p = ao->priv;
    int err;

    if (p->can_pause) {
        if (snd_pcm_state(p->alsa) == SND_PCM_STATE_RUNNING) {
            p->delay_before_pause = get_delay(ao);
            err = snd_pcm_pause(p->alsa, 1);
            CHECK_ALSA_ERROR("pcm pause error");
        }
    } else {
        MP_VERBOSE(ao, "pause not supported by hardware\n");
        if (snd_pcm_delay(p->alsa, &p->prepause_frames) < 0
            || p->prepause_frames < 0)
            p->prepause_frames = 0;
        p->delay_before_pause = p->prepause_frames / (double)ao->samplerate;

        err = snd_pcm_drop(p->alsa);
        CHECK_ALSA_ERROR("pcm drop error");
    }

alsa_error: ;
}

static void audio_resume(struct ao *ao)
{
    struct priv *p = ao->priv;
    int err;

    if (snd_pcm_state(p->alsa) == SND_PCM_STATE_SUSPENDED) {
        MP_INFO(ao, "PCM in suspend mode, trying to resume.\n");

        while ((err = snd_pcm_resume(p->alsa)) == -EAGAIN)
            sleep(1);
    }

    if (p->can_pause) {
        if (snd_pcm_state(p->alsa) == SND_PCM_STATE_PAUSED) {
            err = snd_pcm_pause(p->alsa, 0);
            CHECK_ALSA_ERROR("pcm resume error");
        }
    } else {
        MP_VERBOSE(ao, "resume not supported by hardware\n");
        err = snd_pcm_prepare(p->alsa);
        CHECK_ALSA_ERROR("pcm prepare error");
        if (p->prepause_frames)
            ao_play_silence(ao, p->prepause_frames);
    }

alsa_error: ;
}

static void reset(struct ao *ao)
{
    struct priv *p = ao->priv;
    int err;

    p->prepause_frames = 0;
    p->delay_before_pause = 0;
    err = snd_pcm_drop(p->alsa);
    CHECK_ALSA_ERROR("pcm prepare error");
    err = snd_pcm_prepare(p->alsa);
    CHECK_ALSA_ERROR("pcm prepare error");

alsa_error: ;
}

static int play(struct ao *ao, void **data, int samples, int flags)
{
    struct priv *p = ao->priv;
    snd_pcm_sframes_t res = 0;
    if (!(flags & AOPLAY_FINAL_CHUNK))
        samples = samples / p->outburst * p->outburst;

    if (samples == 0)
        return 0;

    do {
        if (af_fmt_is_planar(ao->format)) {
            res = snd_pcm_writen(p->alsa, data, samples);
        } else {
            res = snd_pcm_writei(p->alsa, data[0], samples);
        }

        if (res == -EINTR || res == -EAGAIN) { /* retry */
            res = 0;
        } else if (res == -ESTRPIPE) {  /* suspend */
            audio_resume(ao);
        } else if (res < 0) {
            MP_ERR(ao, "Write error: %s\n", snd_strerror(res));
            res = snd_pcm_prepare(p->alsa);
            int err = res;
            CHECK_ALSA_ERROR("pcm prepare error");
            res = 0;
        }
    } while (res == 0);

    return res < 0 ? -1 : res;

alsa_error:
    return -1;
}

#define MAX_POLL_FDS 20
static int audio_wait(struct ao *ao, pthread_mutex_t *lock)
{
    struct priv *p = ao->priv;
    int err;

    int num_fds = snd_pcm_poll_descriptors_count(p->alsa);
    if (num_fds <= 0 || num_fds >= MAX_POLL_FDS)
        goto alsa_error;

    struct pollfd fds[MAX_POLL_FDS];
    err = snd_pcm_poll_descriptors(p->alsa, fds, num_fds);
    CHECK_ALSA_ERROR("cannot get pollfds");

    while (1) {
        int r = ao_wait_poll(ao, fds, num_fds, lock);
        if (r)
            return r;

        unsigned short revents;
        snd_pcm_poll_descriptors_revents(p->alsa, fds, num_fds, &revents);
        CHECK_ALSA_ERROR("cannot read poll events");

        if (revents & POLLERR)
            return -1;
        if (revents & POLLOUT)
            return 0;
    }
    return 0;

alsa_error:
    return -1;
}

static void list_devs(struct ao *ao, struct ao_device_list *list)
{
    void **hints;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return;

    for (int n = 0; hints[n]; n++) {
        char *name = snd_device_name_get_hint(hints[n], "NAME");
        char *desc = snd_device_name_get_hint(hints[n], "DESC");
        char *io = snd_device_name_get_hint(hints[n], "IOID");
        if (io && strcmp(io, "Output") != 0)
            continue;
        char desc2[1024];
        snprintf(desc2, sizeof(desc2), "%s", desc ? desc : "");
        for (int i = 0; desc2[i]; i++) {
            if (desc2[i] == '\n')
                desc2[i] = '/';
        }
        ao_device_list_add(list, ao, &(struct ao_device_desc){name, desc2});
    }

    snd_device_name_free_hint(hints);
}

#define OPT_BASE_STRUCT struct priv

const struct ao_driver audio_out_alsa = {
    .description = "ALSA audio output",
    .name      = "alsa",
    .init      = init,
    .uninit    = uninit,
    .control   = control,
    .get_space = get_space,
    .play      = play,
    .get_delay = get_delay,
    .pause     = audio_pause,
    .resume    = audio_resume,
    .reset     = reset,
    .drain     = drain,
    .wait      = audio_wait,
    .wakeup    = ao_wakeup_poll,
    .list_devs = list_devs,
    .priv_size = sizeof(struct priv),
    .priv_defaults = &(const struct priv) {
        .cfg_block = 1,
        .cfg_mixer_device = "default",
        .cfg_mixer_name = "Master",
        .cfg_mixer_index = 0,
        .cfg_ni = 0,
    },
    .options = (const struct m_option[]) {
        OPT_STRING("device", cfg_device, 0),
        OPT_FLAG("resample", cfg_resample, 0),
        OPT_FLAG("block", cfg_block, 0),
        OPT_STRING("mixer-device", cfg_mixer_device, 0),
        OPT_STRING("mixer-name", cfg_mixer_name, 0),
        OPT_INTRANGE("mixer-index", cfg_mixer_index, 0, 0, 99),
        OPT_FLAG("non-interleaved", cfg_ni, 0),
        {0}
    },
};
