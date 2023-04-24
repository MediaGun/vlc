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

#include "mmdevice_volume_control.h"

#include <vlc_aout.h>

typedef struct
{
    audio_output_t *aout;
    struct IAudioSessionEvents session_events;
    ISimpleAudioVolume *volume;
    IAudioSessionControl *control;
    LONG refs;
    HANDLE work_event;
    vlc_mutex_t lock;

    float gain; /**< Current software gain volume */
    bool report_volume;
    bool report_mute;
    float requested_volume; /**< Requested volume, negative if none */
    signed char requested_mute; /**< Requested mute, negative if none */


} mmdevice_volume_sys_t;

/*** Audio session events ***/
static STDMETHODIMP
vlc_AudioSessionEvents_QueryInterface(IAudioSessionEvents *this, REFIID riid,
                                      void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown)
     || IsEqualIID(riid, &IID_IAudioSessionEvents))
    {
        *ppv = this;
        IUnknown_AddRef(this);
        return S_OK;
    }
    else
    {
       *ppv = NULL;
        return E_NOINTERFACE;
    }
}

static STDMETHODIMP_(ULONG)
vlc_AudioSessionEvents_AddRef(IAudioSessionEvents *this)
{
    mmdevice_volume_sys_t *sys = container_of(this, mmdevice_volume_sys_t, session_events);
    return InterlockedIncrement(&sys->refs);
}

static STDMETHODIMP_(ULONG)
vlc_AudioSessionEvents_Release(IAudioSessionEvents *this)
{
    mmdevice_volume_sys_t *sys = container_of(this, mmdevice_volume_sys_t, session_events);
    return InterlockedDecrement(&sys->refs);
}

static STDMETHODIMP
vlc_AudioSessionEvents_OnDisplayNameChanged(IAudioSessionEvents *this,
                                            LPCWSTR wname, LPCGUID ctx)
{
    mmdevice_volume_sys_t *sys = container_of(this, mmdevice_volume_sys_t, session_events);
    audio_output_t *aout = sys->aout;

    msg_Dbg(aout, "display name changed: %ls", wname);
    (void) ctx;
    return S_OK;
}

static STDMETHODIMP
vlc_AudioSessionEvents_OnIconPathChanged(IAudioSessionEvents *this,
                                         LPCWSTR wpath, LPCGUID ctx)
{
    mmdevice_volume_sys_t *sys = container_of(this, mmdevice_volume_sys_t, session_events);
    audio_output_t *aout = sys->aout;

    msg_Dbg(aout, "icon path changed: %ls", wpath);
    (void) ctx;
    return S_OK;
}

static STDMETHODIMP
vlc_AudioSessionEvents_OnSimpleVolumeChanged(IAudioSessionEvents *this,
                                             float vol, BOOL mute,
                                             LPCGUID ctx)
{
    mmdevice_volume_sys_t *sys = container_of(this, mmdevice_volume_sys_t, session_events);
    audio_output_t *aout = sys->aout;

    msg_Dbg(aout, "simple volume changed: %f, muting %sabled", vol,
            mute ? "en" : "dis");

    vlc_mutex_lock(&sys->lock);
    sys->report_volume = true;
    sys->report_mute = true;
    vlc_mutex_unlock(&sys->lock);
    SetEvent(sys->work_event);

    (void) ctx;
    return S_OK;
}

static STDMETHODIMP
vlc_AudioSessionEvents_OnChannelVolumeChanged(IAudioSessionEvents *this,
                                              DWORD count, float *vols,
                                              DWORD changed, LPCGUID ctx)
{
    mmdevice_volume_sys_t *sys = container_of(this, mmdevice_volume_sys_t, session_events);
    audio_output_t *aout = sys->aout;

    if (changed != (DWORD)-1)
        msg_Dbg(aout, "channel volume %lu of %lu changed: %f", changed, count,
                vols[changed]);
    else
        msg_Dbg(aout, "%lu channels volume changed", count);

    (void) ctx;
    return S_OK;
}

static STDMETHODIMP
vlc_AudioSessionEvents_OnGroupingParamChanged(IAudioSessionEvents *this,
                                              LPCGUID param, LPCGUID ctx)

{
    mmdevice_volume_sys_t *sys = container_of(this, mmdevice_volume_sys_t, session_events);
    audio_output_t *aout = sys->aout;

    msg_Dbg(aout, "grouping parameter changed");
    (void) param;
    (void) ctx;
    return S_OK;
}

static STDMETHODIMP
vlc_AudioSessionEvents_OnStateChanged(IAudioSessionEvents *this,
                                      AudioSessionState state)
{
    mmdevice_volume_sys_t *sys = container_of(this, mmdevice_volume_sys_t, session_events);
    audio_output_t *aout = sys->aout;

    msg_Dbg(aout, "state changed: %d", state);
    return S_OK;
}

static STDMETHODIMP
vlc_AudioSessionEvents_OnSessionDisconnected(IAudioSessionEvents *this,
                                           AudioSessionDisconnectReason reason)
{
    mmdevice_volume_sys_t *sys = container_of(this, mmdevice_volume_sys_t, session_events);
    audio_output_t *aout = sys->aout;

    switch (reason)
    {
        case DisconnectReasonDeviceRemoval:
            msg_Warn(aout, "session disconnected: %s", "device removed");
            break;
        case DisconnectReasonServerShutdown:
            msg_Err(aout, "session disconnected: %s", "service stopped");
            return S_OK;
        case DisconnectReasonFormatChanged:
            msg_Warn(aout, "session disconnected: %s", "format changed");
            break;
        case DisconnectReasonSessionLogoff:
            msg_Err(aout, "session disconnected: %s", "user logged off");
            return S_OK;
        case DisconnectReasonSessionDisconnected:
            msg_Err(aout, "session disconnected: %s", "session disconnected");
            return S_OK;
        case DisconnectReasonExclusiveModeOverride:
            msg_Err(aout, "session disconnected: %s", "stream overridden");
            return S_OK;
        default:
            msg_Warn(aout, "session disconnected: unknown reason %d", reason);
            return S_OK;
    }
    /* NOTE: audio decoder thread should get invalidated device and restart */
    return S_OK;
}

static const struct IAudioSessionEventsVtbl vlc_AudioSessionEvents =
{
    vlc_AudioSessionEvents_QueryInterface,
    vlc_AudioSessionEvents_AddRef,
    vlc_AudioSessionEvents_Release,

    vlc_AudioSessionEvents_OnDisplayNameChanged,
    vlc_AudioSessionEvents_OnIconPathChanged,
    vlc_AudioSessionEvents_OnSimpleVolumeChanged,
    vlc_AudioSessionEvents_OnChannelVolumeChanged,
    vlc_AudioSessionEvents_OnGroupingParamChanged,
    vlc_AudioSessionEvents_OnStateChanged,
    vlc_AudioSessionEvents_OnSessionDisconnected,
};

static void Process(mmdevice_volume_controler_t *controler, struct aout_stream_owner * stream)
{
    VLC_UNUSED(stream);
    HRESULT hr;
    mmdevice_volume_sys_t *sys = controler->sys;
    audio_output_t *aout = sys->aout;

    vlc_mutex_lock(&sys->lock);

    if (sys->requested_volume >= 0.f)
    {
        hr = ISimpleAudioVolume_SetMasterVolume(sys->volume, sys->requested_volume, NULL);
        if (FAILED(hr))
            msg_Err(aout, "cannot set master volume (error 0x%lX)", hr);
        sys->requested_volume = -1.f;
    }


    if (sys->report_volume)
    {
        float level;
        hr = ISimpleAudioVolume_GetMasterVolume(sys->volume, &level);
        if (SUCCEEDED(hr))
            aout_VolumeReport(aout, cbrtf(level * sys->gain));
        else
            msg_Err(aout, "cannot get master volume (error 0x%lX)", hr);

        sys->report_volume = false;
    }

    if (sys->requested_mute >= 0)
    {
        BOOL mute = sys->requested_mute ? TRUE : FALSE;

        hr = ISimpleAudioVolume_SetMute(sys->volume, mute, NULL);
        if (FAILED(hr))
            msg_Err(aout, "cannot set mute (error 0x%lX)", hr);
        sys->requested_mute = -1;
    }

    if (sys->report_mute)
    {
        BOOL mute;
        hr = ISimpleAudioVolume_GetMute(sys->volume, &mute);
        if (SUCCEEDED(hr))
            aout_MuteReport(aout, mute != FALSE);
        else
            msg_Err(aout, "cannot get mute (error 0x%lX)", hr);

        sys->report_mute = false;
    }

     vlc_mutex_unlock(&sys->lock);
}

static int RequestMute(mmdevice_volume_controler_t *controler, bool mute)
{
    mmdevice_volume_sys_t *sys = controler->sys;

    vlc_mutex_lock(&sys->lock);
    {
        sys->requested_mute = mute;
        sys->report_mute = true;
    }
    vlc_mutex_unlock(&sys->lock);

    SetEvent(sys->work_event);
    return 0;
}

static int RequestVolume(mmdevice_volume_controler_t *controler, float vol, float* outGain)
{
    mmdevice_volume_sys_t *sys = controler->sys;

    float gain = 1.f;

    vol = vol * vol * vol; /* ISimpleAudioVolume is tapered linearly. */

    if (vol > 1.f)
    {
        gain = vol;
        vol = 1.f;
    }

    vlc_mutex_lock(&sys->lock);
    {
        sys->requested_volume = vol;
        sys->report_volume = true;
        sys->gain =gain;
    }
    vlc_mutex_unlock(&sys->lock);

    if (outGain != NULL)
        *outGain = gain;
    SetEvent(sys->work_event);

    return 0;
}

static void Cleanup(mmdevice_volume_controler_t *controler)
{
    mmdevice_volume_sys_t *sys = controler->sys;
    if (!sys)
        return;

    if (sys->volume != NULL)
        ISimpleAudioVolume_Release(sys->volume);

    if (sys->control)
    {
        IAudioSessionControl_UnregisterAudioSessionNotification(sys->control,
                                                     &sys->session_events);
        IAudioSessionControl_Release(sys->control);
    }

    free(sys);
}

static bool Initialize(struct mmdevice_volume_controler_t* volume, IAudioSessionManager *manager, LPCGUID guid)
{
    mmdevice_volume_sys_t *sys = volume->sys;

    HRESULT hr;

    IAudioSessionControl *control;
    hr = IAudioSessionManager_GetAudioSessionControl(manager, guid, 0,
                                                     &control);
    if (FAILED(hr))
        return false;

    sys->control = control;

    IAudioSessionControl_RegisterAudioSessionNotification(control,
                                                 &sys->session_events);


    hr = IAudioSessionManager_GetSimpleAudioVolume(manager, guid, FALSE,
                                                   &sys->volume);
    if (FAILED(hr))
    {
        msg_Err(sys->aout, "cannot get simple volume (error 0x%lx)", hr);
        return false;
    }

    return true;
}

mmdevice_volume_controler_t* createMMDeviceSessionVolumeControler(audio_output_t *aout, HANDLE workevent)
{

    mmdevice_volume_controler_t* volume = malloc(sizeof(mmdevice_volume_controler_t));
    if (!volume)
        return NULL;

    mmdevice_volume_sys_t *sys = malloc(sizeof(mmdevice_volume_sys_t));
    if (!sys)
    {
        free(volume);
        return NULL;
    }

    sys->aout = aout;
    sys->refs = 1;
    sys->volume = NULL;
    sys->control = NULL;
    sys->gain = 1.f;
    sys->work_event = workevent;
    vlc_mutex_init(&sys->lock);

    sys->report_volume = true;
    sys->report_mute = true;
    sys->requested_volume = -1.f;
    sys->requested_mute = -1;

    sys->session_events.lpVtbl = &vlc_AudioSessionEvents;

    volume->sys = sys;
    volume->cleanup = Cleanup;
    volume->requestMute = RequestMute;
    volume->requestVolume = RequestVolume;

    volume->initialize = Initialize;
    volume->process = Process;

    return volume;
}


