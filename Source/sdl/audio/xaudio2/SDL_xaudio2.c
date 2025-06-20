/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2014 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#if SDL_AUDIO_DRIVER_XAUDIO2

#include "../../core/windows/SDL_windows.h"
#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "../SDL_sysaudio.h"
#include "SDL_assert.h"

/* Use embedded xaudio2 definitions to avoid library dependency */
#include "SDL_xaudio2.h"
#define XAUDIO2_E_DEVICE_INVALIDATED 0x88960004

/* Hidden "this" pointer for the audio functions */
#define _THIS   SDL_AudioDevice *this

/* Fixes bug 1210 where some versions of gcc need named parameters */
#ifdef __GNUC__
#ifdef THIS
#undef THIS
#endif
#define THIS    INTERFACE *p
#ifdef THIS_
#undef THIS_
#endif
#define THIS_   INTERFACE *p,
#endif

struct SDL_PrivateAudioData
{
    IXAudio2_Generic *ixa2;
    IXAudio2SourceVoice *source;
    IXAudio2MasteringVoice *mastering;
    SDL_sem * semaphore;
    Uint8 *mixbuf;
    int mixlen;
    SDL_bool is29;
    Uint8 *nextbuf;
};


static void
XAUDIO2_DetectDevices(int iscapture, SDL_AddAudioDevice addfn)
{
    IXAudio2_Generic *ixa2 = NULL;
    UINT32 devcount = 0;
    UINT32 i = 0;
    SDL_bool is29;

    if (iscapture) {
        SDL_SetError("XAudio2: capture devices unsupported.");
        return;
    } else if (SDL_XAudio2Create(&ixa2, 0, XAUDIO2_DEFAULT_PROCESSOR, &is29) != S_OK) {
        SDL_SetError("XAudio2: XAudio2Create() failed at detection.");
        return;
    }

#if 0 /* only 2.7 can do this, so it is disabled */
    if (IXAudio2_GetDeviceCount(ixa2, &devcount) != S_OK) {
        SDL_SetError("XAudio2: IXAudio2::GetDeviceCount() failed.");
        IXAudio2_Release(ixa2);
        return;
    }

    for (i = 0; i < devcount; i++) {
        XAUDIO2_DEVICE_DETAILS details;
        if (IXAudio2_GetDeviceDetails(ixa2, i, &details) == S_OK) {
            char *str = WIN_StringToUTF8(details.DisplayName);
            if (str != NULL) {
                addfn(str);
                SDL_free(str);  /* addfn() made a copy of the string. */
            }
        }
    }
#else
    addfn("XAudio2");
#endif

    if (is29) IXAudio2_29_Release(ixa2); else IXAudio2_27_Release(ixa2);
}

#undef INTERFACE
#define INTERFACE IXAudio2VoiceCallback

static void STDMETHODCALLTYPE
VoiceCBOnBufferEnd(THIS_ void *data)
{
    /* Just signal the SDL audio thread and get out of XAudio2's way. */
    SDL_AudioDevice *this = (SDL_AudioDevice *) data;
    SDL_SemPost(this->hidden->semaphore);
}

static void STDMETHODCALLTYPE
VoiceCBOnVoiceError(THIS_ void *data, HRESULT Error)
{
    /* !!! FIXME: attempt to recover, or mark device disconnected. */
    SDL_assert(0 && "write me!");
}

/* no-op callbacks... */
static void STDMETHODCALLTYPE VoiceCBOnStreamEnd(THIS) {}
static void STDMETHODCALLTYPE VoiceCBOnVoiceProcessPassStart(THIS_ UINT32 b) {}
static void STDMETHODCALLTYPE VoiceCBOnVoiceProcessPassEnd(THIS) {}
static void STDMETHODCALLTYPE VoiceCBOnBufferStart(THIS_ void *data) {}
static void STDMETHODCALLTYPE VoiceCBOnLoopEnd(THIS_ void *data) {}

static Uint8 *
XAUDIO2_GetDeviceBuf(_THIS)
{
    return this->hidden->nextbuf;
}

static void
XAUDIO2_PlayDevice(_THIS)
{
    XAUDIO2_BUFFER buffer;
    Uint8 *mixbuf = this->hidden->mixbuf;
    Uint8 *nextbuf = this->hidden->nextbuf;
    const int mixlen = this->hidden->mixlen;
    IXAudio2SourceVoice *source = this->hidden->source;
    HRESULT result = S_OK;

    if (!this->enabled) { /* shutting down? */
        return;
    }

    /* Submit the next filled buffer */
    SDL_zero(buffer);
    buffer.AudioBytes = mixlen;
    buffer.pAudioData = nextbuf;
    buffer.pContext = this;

    if (nextbuf == mixbuf) {
        nextbuf += mixlen;
    } else {
        nextbuf = mixbuf;
    }
    this->hidden->nextbuf = nextbuf;

    result = IXAudio2SourceVoice_SubmitSourceBuffer(source, &buffer, NULL);
    if (result == XAUDIO2_E_DEVICE_INVALIDATED) {
        /* !!! FIXME: possibly disconnected or temporary lost. Recover? */
    }

    if (result != S_OK) {  /* uhoh, panic! */
        IXAudio2SourceVoice_FlushSourceBuffers(source);
        this->enabled = 0;
    }
}

static void
XAUDIO2_WaitDevice(_THIS)
{
    if (this->enabled) {
        SDL_SemWait(this->hidden->semaphore);
    }
}

static void
XAUDIO2_WaitDone(_THIS)
{
    IXAudio2SourceVoice *source = this->hidden->source;
    XAUDIO2_VOICE_STATE state;
    SDL_assert(!this->enabled);  /* flag that stops playing. */
    IXAudio2SourceVoice_Discontinuity(source);
    IXAudio2SourceVoice_GetState(this->hidden->is29, source, &state);
    while (state.BuffersQueued > 0) {
        SDL_SemWait(this->hidden->semaphore);
        IXAudio2SourceVoice_GetState(this->hidden->is29, source, &state);
    }
}

static void
XAUDIO2_CloseDevice(_THIS)
{
    if (this->hidden != NULL) {
        IXAudio2_Generic *ixa2 = this->hidden->ixa2;
        IXAudio2SourceVoice *source = this->hidden->source;
        IXAudio2MasteringVoice *mastering = this->hidden->mastering;

        if (source != NULL) {
            IXAudio2SourceVoice_Stop(source, 0, XAUDIO2_COMMIT_NOW);
            IXAudio2SourceVoice_FlushSourceBuffers(source);
            IXAudio2SourceVoice_DestroyVoice(source);
        }
        if (ixa2 != NULL) {
            if (this->hidden->is29) IXAudio2_29_StopEngine(ixa2); else IXAudio2_27_StopEngine(ixa2);
        }
        if (mastering != NULL) {
            IXAudio2MasteringVoice_DestroyVoice(mastering);
        }
        if (ixa2 != NULL) {
            if (this->hidden->is29) IXAudio2_29_Release(ixa2); else IXAudio2_27_Release(ixa2);
        }
        SDL_free(this->hidden->mixbuf);
        if (this->hidden->semaphore != NULL) {
            SDL_DestroySemaphore(this->hidden->semaphore);
        }

        SDL_free(this->hidden);
        this->hidden = NULL;
    }
}

static int
XAUDIO2_OpenDevice(_THIS, const char *devname, int iscapture)
{
    HRESULT result = S_OK;
    WAVEFORMATEX waveformat;
    int valid_format = 0;
    SDL_AudioFormat test_format = SDL_FirstAudioFormat(this->spec.format);
    IXAudio2_Generic *ixa2 = NULL;
    IXAudio2SourceVoice *source = NULL;
    UINT32 devId = 0;  /* 0 == system default device. */
    SDL_bool is29;

    static IXAudio2VoiceCallbackVtbl callbacks_vtable = {
        VoiceCBOnVoiceProcessPassStart,
        VoiceCBOnVoiceProcessPassEnd,
        VoiceCBOnStreamEnd,
        VoiceCBOnBufferStart,
        VoiceCBOnBufferEnd,
        VoiceCBOnLoopEnd,
        VoiceCBOnVoiceError
    };

    static IXAudio2VoiceCallback callbacks = { &callbacks_vtable };

    if (iscapture) {
        return SDL_SetError("XAudio2: capture devices unsupported.");
    } else if (SDL_XAudio2Create(&ixa2, 0, XAUDIO2_DEFAULT_PROCESSOR, &is29) != S_OK) {
        return SDL_SetError("XAudio2: XAudio2Create() failed at open.");
    }

    /*
    XAUDIO2_DEBUG_CONFIGURATION debugConfig;
    debugConfig.TraceMask = XAUDIO2_LOG_ERRORS; //XAUDIO2_LOG_WARNINGS | XAUDIO2_LOG_DETAIL | XAUDIO2_LOG_FUNC_CALLS | XAUDIO2_LOG_TIMING | XAUDIO2_LOG_LOCKS | XAUDIO2_LOG_MEMORY | XAUDIO2_LOG_STREAMING;
    debugConfig.BreakMask = XAUDIO2_LOG_ERRORS; //XAUDIO2_LOG_WARNINGS;
    debugConfig.LogThreadID = TRUE;
    debugConfig.LogFileline = TRUE;
    debugConfig.LogFunctionName = TRUE;
    debugConfig.LogTiming = TRUE;
    ixa2->SetDebugConfiguration(&debugConfig);
    */

#if 0 /* only 2.7 can do this, so it is disabled */
    if (devname != NULL) {
        UINT32 devcount = 0;
        UINT32 i = 0;

        if (IXAudio2_GetDeviceCount(ixa2, &devcount) != S_OK) {
            IXAudio2_Release(ixa2);
            return SDL_SetError("XAudio2: IXAudio2_GetDeviceCount() failed.");
        }
        for (i = 0; i < devcount; i++) {
            XAUDIO2_DEVICE_DETAILS details;
            if (IXAudio2_GetDeviceDetails(ixa2, i, &details) == S_OK) {
                char *str = WIN_StringToUTF8(details.DisplayName);
                if (str != NULL) {
                    const int match = (SDL_strcmp(str, devname) == 0);
                    SDL_free(str);
                    if (match) {
                        devId = i;
                        break;
                    }
                }
            }
        }

        if (i == devcount) {
            IXAudio2_Release(ixa2);
            return SDL_SetError("XAudio2: Requested device not found.");
        }
    }
#endif

    /* Initialize all variables that we clean on shutdown */
    this->hidden = (struct SDL_PrivateAudioData *)
        SDL_malloc((sizeof *this->hidden));
    if (this->hidden == NULL) {
        if (is29) IXAudio2_29_Release(ixa2); else IXAudio2_27_Release(ixa2);
        return SDL_OutOfMemory();
    }
    SDL_memset(this->hidden, 0, (sizeof *this->hidden));

    this->hidden->ixa2 = ixa2;
    this->hidden->is29 = is29;
    this->hidden->semaphore = SDL_CreateSemaphore(1);
    if (this->hidden->semaphore == NULL) {
        XAUDIO2_CloseDevice(this);
        return SDL_SetError("XAudio2: CreateSemaphore() failed!");
    }

    while ((!valid_format) && (test_format)) {
        switch (test_format) {
        case AUDIO_U8:
        case AUDIO_S16:
        case AUDIO_S32:
        case AUDIO_F32:
            this->spec.format = test_format;
            valid_format = 1;
            break;
        }
        test_format = SDL_NextAudioFormat();
    }

    if (!valid_format) {
        XAUDIO2_CloseDevice(this);
        return SDL_SetError("XAudio2: Unsupported audio format");
    }

    /* Update the fragment size as size in bytes */
    SDL_CalculateAudioSpec(&this->spec);

    /* We feed a Source, it feeds the Mastering, which feeds the device. */
    this->hidden->mixlen = this->spec.size;
    this->hidden->mixbuf = (Uint8 *) SDL_malloc(2 * this->hidden->mixlen);
    if (this->hidden->mixbuf == NULL) {
        XAUDIO2_CloseDevice(this);
        return SDL_OutOfMemory();
    }
    this->hidden->nextbuf = this->hidden->mixbuf;
    SDL_memset(this->hidden->mixbuf, 0, 2 * this->hidden->mixlen);

    /* We use XAUDIO2_DEFAULT_CHANNELS instead of this->spec.channels. On
       Xbox360, this means 5.1 output, but on Windows, it means "figure out
       what the system has." It might be preferable to let XAudio2 blast
       stereo output to appropriate surround sound configurations
       instead of clamping to 2 channels, even though we'll configure the
       Source Voice for whatever number of channels you supply. */
    if (is29) result = IXAudio2_29_CreateMasteringVoice(ixa2, &this->hidden->mastering, XAUDIO2_DEFAULT_CHANNELS, this->spec.freq, 0, NULL, NULL, AudioCategory_GameEffects);
    else      result = IXAudio2_27_CreateMasteringVoice(ixa2, &this->hidden->mastering, XAUDIO2_DEFAULT_CHANNELS, this->spec.freq, 0, devId, NULL);
    if (result != S_OK) {
        XAUDIO2_CloseDevice(this);
        return SDL_SetError("XAudio2: Couldn't create mastering voice");
    }

    SDL_zero(waveformat);
    if (SDL_AUDIO_ISFLOAT(this->spec.format)) {
        waveformat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    } else {
        waveformat.wFormatTag = WAVE_FORMAT_PCM;
    }
    waveformat.wBitsPerSample = SDL_AUDIO_BITSIZE(this->spec.format);
    waveformat.nChannels = this->spec.channels;
    waveformat.nSamplesPerSec = this->spec.freq;
    waveformat.nBlockAlign =
        waveformat.nChannels * (waveformat.wBitsPerSample / 8);
    waveformat.nAvgBytesPerSec =
        waveformat.nSamplesPerSec * waveformat.nBlockAlign;
    waveformat.cbSize = sizeof(waveformat);

    if (is29) result = IXAudio2_29_CreateSourceVoice(ixa2, &source, &waveformat, XAUDIO2_VOICE_NOSRC | XAUDIO2_VOICE_NOPITCH, 1.0f, &callbacks, NULL, NULL);
    else      result = IXAudio2_27_CreateSourceVoice(ixa2, &source, &waveformat, XAUDIO2_VOICE_NOSRC | XAUDIO2_VOICE_NOPITCH, 1.0f, &callbacks, NULL, NULL);

    if (result != S_OK) {
        XAUDIO2_CloseDevice(this);
        return SDL_SetError("XAudio2: Couldn't create source voice");
    }
    this->hidden->source = source;

    /* Start everything playing! */
    if (is29) result = IXAudio2_29_StartEngine(ixa2); else result = IXAudio2_27_StartEngine(ixa2);
    if (result != S_OK) {
        XAUDIO2_CloseDevice(this);
        return SDL_SetError("XAudio2: Couldn't start engine");
    }

    result = IXAudio2SourceVoice_Start(source, 0, XAUDIO2_COMMIT_NOW);
    if (result != S_OK) {
        XAUDIO2_CloseDevice(this);
        return SDL_SetError("XAudio2: Couldn't start source voice");
    }

    return 0; /* good to go. */
}

static void
XAUDIO2_Deinitialize(void)
{
#if defined(__WIN32__)
    WIN_CoUninitialize();
#endif
}

static int
XAUDIO2_Init(SDL_AudioDriverImpl * impl)
{
    /* XAudio2Create() is a macro that uses COM; we don't load the .dll */
    IXAudio2_Generic *ixa2 = NULL;
    SDL_bool is29;

    if (SDL_XAudio2Create(&ixa2, 0, XAUDIO2_DEFAULT_PROCESSOR, &is29) != S_OK) {
        SDL_SetError("XAudio2: XAudio2Create() failed at initialization");
        return 0;  /* not available. */
    }
    if (is29) IXAudio2_29_Release(ixa2); else IXAudio2_27_Release(ixa2);

    /* Set the function pointers */
    impl->DetectDevices = XAUDIO2_DetectDevices;
    impl->OpenDevice = XAUDIO2_OpenDevice;
    impl->PlayDevice = XAUDIO2_PlayDevice;
    impl->WaitDevice = XAUDIO2_WaitDevice;
    impl->WaitDone = XAUDIO2_WaitDone;
    impl->GetDeviceBuf = XAUDIO2_GetDeviceBuf;
    impl->CloseDevice = XAUDIO2_CloseDevice;
    impl->Deinitialize = XAUDIO2_Deinitialize;

    return 1;   /* this audio target is available. */
}

AudioBootStrap XAUDIO2_bootstrap = {
    "xaudio2", "XAudio2", XAUDIO2_Init, 0
};

#endif  /* SDL_AUDIO_DRIVER_XAUDIO2 */

/* vi: set ts=4 sw=4 expandtab: */
