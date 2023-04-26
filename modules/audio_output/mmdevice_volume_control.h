/*****************************************************************************
 * Copyright (C) 2012-2023 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifndef MMDEVICE_VOLUME_H
#define MMDEVICE_VOLUME_H

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <vlc_common.h>

#define INITGUID
#define COBJMACROS
#define CONST_VTABLE

#include <endpointvolume.h>
#include <audiopolicy.h>

struct aout_stream_owner;

struct mmdevice_volume_controler_t
{
    void *sys;

    void (*cleanup)(struct mmdevice_volume_controler_t* volume);

    int (*requestMute)(struct mmdevice_volume_controler_t* volume, bool mute);
    int (*requestVolume)(struct mmdevice_volume_controler_t* volume, float vol, float* gain);

    //initialize is called from the MM Thread
    bool (*initialize)(struct mmdevice_volume_controler_t* volume, IAudioSessionManager *manager, LPCGUID guid);
    //process is called from the MM Thread
    void (*process)(struct mmdevice_volume_controler_t* volume, struct aout_stream_owner * stream);
};

typedef struct mmdevice_volume_controler_t mmdevice_volume_controler_t;

inline static void mmdevice_volume_controler_release(mmdevice_volume_controler_t* controler)
{
    controler->cleanup(controler);
    free(controler);
}

inline static int mmdevice_volume_controler_request_mute(mmdevice_volume_controler_t* controler, bool mute)
{
    return controler->requestMute(controler, mute);
}

inline static int mmdevice_volume_controler_request_volume(mmdevice_volume_controler_t* controler, float vol, float* gain)
{
    return controler->requestVolume(controler, vol, gain);
}

inline static bool mmdevice_volume_controler_initialize(mmdevice_volume_controler_t* controler, IAudioSessionManager *manager, LPCGUID guid)
{
    return controler->initialize(controler, manager, guid);
}

inline static void mmdevice_volume_controler_process(mmdevice_volume_controler_t* controler, struct aout_stream_owner * stream)
{
    controler->process(controler, stream);
}

mmdevice_volume_controler_t* createMMDeviceSessionVolumeControler(audio_output_t *aout, HANDLE work_event);

mmdevice_volume_controler_t* createMMDeviceTrackVolumeControler(audio_output_t *aout, HANDLE work_event);

#endif /* MMDEVICE_VOLUME_H */
