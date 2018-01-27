/*
    Portaudio output plugin for DeaDBeeF Player
    Copyright (C) 2017 Jakub Wasylków

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
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <stdio.h>
#include <string.h>

#include <deadbeef/deadbeef.h>
#include <portaudio.h>

#ifdef __MINGW32__
#include "windows.h"
#endif

#define DEFAULT_BUFFER_SIZE 8192
#define DEFAULT_BUFFER_SIZE_STR "8192"

#define trace(...) {deadbeef->log (__VA_ARGS__);}
//#define trace(...) { fprintf(stdout, __VA_ARGS__); }
//#define trace(...) { deadbeef->log_detailed (&plugin.plugin, 1, __VA_ARGS__); }
#define warn(...) {deadbeef->log (__VA_ARGS__);}
#define info(...) { deadbeef->log_detailed (&plugin.plugin, 1, __VA_ARGS__); }

// This is my 3rd rewrite of portaudio plugin. Hopefully last.

static PaSampleFormat
pa_GetSampleFormat (int bps, int is_float);

static void
pa_SetDefault ();

static int
portaudio_open_stream (void);

static int
portaudio_free (void);

static int
portaudio_setformat (ddb_waveformat_t *fmt);

static int
portaudio_play (void);

static int
portaudio_stop (void);

static int
portaudio_pause (void);

static int
portaudio_unpause (void);

static int
portaudio_callback (const void *inputBuffer, void *outputBuffer,
                    unsigned long framesPerBuffer,
                    const PaStreamCallbackTimeInfo* timeInfo,
                    PaStreamCallbackFlags statusFlags,
                    void *userData );

static void
pa_stream_finished_callback (void *uData);

static DB_output_t plugin;
DB_functions_t *deadbeef;
static uintptr_t mutex;

static int state;

// actual stream
static PaStream *stream = 0;
// we use plugin.fmt as fmt information
static PaStreamParameters stream_parameters;
// pointer which tells callback to abort stream
static struct uData * userData;
// old userData pointer, tells if stream got aborted
static struct uData * userDataOld;

// requested stream
static ddb_waveformat_t requested_fmt;

// previous stream (needs to be closed after stream abort)
static PaStream *previous_stream = 0;

#define STREAM_CONTINUE 0
#define STREAM_COMPLETE 1
#define STREAM_ABORT 2
#define STREAM_SWITCH_WAIT 3
#define STREAM_FINISHED 4
#define STREAM_MURDER 5


struct uData {
    int function;
    int num;
    PaStream * stream_to_kill;
    struct uData * stream_to_kill_udata;
};

unsigned char num_assign = 0;

// portaudio_open_stream opens a stream using stream_parameters and plugin.fmt information
static int
portaudio_open_stream (void) {
    trace ("portaudio_open_stream\n");

    deadbeef->mutex_lock (mutex);

    // Use default device if none selected
    if (stream_parameters.device == -1) {
        stream_parameters.device = Pa_GetDefaultOutputDevice ();
    }

    if (userData)
        warn ("portaudio_open_stream: WARN: userData is not empty!\n");
    userData = calloc(1,sizeof(struct uData));
    userData->num = num_assign++;

    // Using paFramesPerBufferUnspecified with alsa gives warnings about underrun
    int buffer_size_config = deadbeef->conf_get_int ("portaudio.buffer", DEFAULT_BUFFER_SIZE);
    unsigned long buffer_size;
    if (buffer_size_config == -1)
        buffer_size = paFramesPerBufferUnspecified;
    else
        buffer_size = buffer_size_config;

    trace ("portaudio_open_stream [%d]: buffer size %lu\n", userData->num, buffer_size);
    /* Open an audio I/O stream. */
    PaError err;
    err = Pa_OpenStream (       &stream,                        // stream pointer
                                NULL,                           // inputParameters
                                &stream_parameters,             // outputParameters
                                plugin.fmt.samplerate,          // sampleRate
                                buffer_size,                    // framesPerBuffer
                                paNoFlag,                       // flags
                                portaudio_callback,             // callback
                                userData);                      // userData 

    if (err != paNoError) {
        trace("Failed to open stream. %s\n", Pa_GetErrorText(err));
        deadbeef->mutex_unlock (mutex);
        return -1;
    }
    err = Pa_SetStreamFinishedCallback (stream, pa_stream_finished_callback);
    if (err != paNoError) {
        trace ("Failed to set stream finished callback. %s\n", Pa_GetErrorText(err));
    }
    deadbeef->mutex_unlock (mutex);
    return 0;
}

static int
portaudio_init (void) {
    // this function is for now left for no reason
    trace ("portaudio_init\n");
    //Pa_Sleep (1000);
    return 0;
}

static void
pa_stream_finished_callback (void *uData) {
    trace ("pa_stream_finished_callback\n");
    if (userData)
        trace ("pa_stream_finished_callback: set userdata to STREAM_CONTINUE\n");
        userData->function = STREAM_CONTINUE;
    return;
}

// since we can't change stream parameters, we have to abort actual stream, set values and start streaming again
static int
portaudio_setformat (ddb_waveformat_t *fmt) {
    memcpy (&requested_fmt, fmt, sizeof (ddb_waveformat_t));
    trace ("portaudio_setformat %dbit %s %dch %dHz channelmask=%X\n", requested_fmt.bps, fmt->is_float ? "float" : "int", fmt->channels, fmt->samplerate, fmt->channelmask);

    if (!memcmp (&requested_fmt, &plugin.fmt, sizeof (ddb_waveformat_t))) {
        trace ("portaudio_setformat ignored\n");
        return 0;
    }
    else {
        trace ("switching format: (requested->actual)\n"
        "bps %d -> %d\n"
        "is_float %d -> %d\n"
        "channels %d -> %d\n"
        "samplerate %d -> %d\n"
        "channelmask %d -> %d\n"
        , fmt->bps, plugin.fmt.bps
        , fmt->is_float, plugin.fmt.is_float
        , fmt->channels, plugin.fmt.channels
        , fmt->samplerate, plugin.fmt.samplerate
        , fmt->channelmask, plugin.fmt.channelmask
        );
    }

    // Tell ongoing callback 'thread' to abort stream (if any)
    // stream will be closed when new 'thread' will open

    // set userdata if necessary and move it to userdata old
    if (userData) {
        userData->function = STREAM_COMPLETE;
        if (userDataOld) {
            userData->stream_to_kill = previous_stream;
            userData->stream_to_kill_udata = userDataOld;
        }
        userDataOld = userData;
        userData = 0;
    }

    memcpy (&plugin.fmt, &requested_fmt, sizeof (ddb_waveformat_t));
    
    // Set new values for new stream
    // TODO: get which device was requested?
    PaError err;
    stream_parameters.device = Pa_GetDefaultOutputDevice ();
    stream_parameters.channelCount = plugin.fmt.channels;
    stream_parameters.sampleFormat = pa_GetSampleFormat (plugin.fmt.bps,plugin.fmt.is_float);
    stream_parameters.suggestedLatency = 0.0;
    stream_parameters.hostApiSpecificStreamInfo = NULL;

    
    err = Pa_IsFormatSupported (NULL, &stream_parameters, plugin.fmt.samplerate);
    if (err != paNoError) {
        trace ("Unsupported format. %s\n", Pa_GetErrorText(err));
        // even if it failed -- continue
    }

    // start new stream if was playing
    if (stream){
        previous_stream = stream;
        stream = 0;
        portaudio_open_stream ();
        state = OUTPUT_STATE_PLAYING;
        userData->function = STREAM_SWITCH_WAIT;
        err = Pa_StartStream (stream);
        if (err != paNoError) {
            trace ("Failed to start stream. %s\n", Pa_GetErrorText(err));
            state = OUTPUT_STATE_STOPPED;
            return -1;
        }
    }
    return 0;

}

static int
portaudio_free (void) {
    // called when plugin changes
    trace("portaudio_free\n");
    portaudio_stop ();
    return 0;
}

static int
portaudio_play (void) {
    trace ("portaudio_play\n");
    if (!stream) {
        trace ("portaudio_play: opening stream\n");
        portaudio_open_stream ();
    }

    state = OUTPUT_STATE_PLAYING;

    if (!Pa_IsStreamActive(stream)){
        PaError err;
        err = Pa_StartStream (stream);
        if (err != paNoError) {
            trace ("Failed to start stream. %s\n", Pa_GetErrorText(err));
            state = OUTPUT_STATE_STOPPED;
            return -1;
        }
        trace ("portaudio_play: Started stream.\n");
    }
    return 0;
}

static PaSampleFormat
pa_GetSampleFormat (int bps, int is_float) {
    switch (bps) {
    case 8:
        return paUInt8;
        break;
    case 16:
        return paInt16;
        break;
    case 24:
        return paInt24;
        break;
    case 32:
        if (is_float)
            return paFloat32;
        else
            return paInt32;
        break;
    default:
        trace("portaudio: Sample format wrong? Using Int16.\n");
        break;
    };
    return paInt16;
}

static int
portaudio_stop (void) {
    trace ("portaudio_stop\n");
    if (state == OUTPUT_STATE_STOPPED) {
        return -1;
    }
    if (stream) {
        PaError err;
        if (Pa_IsStreamActive(stream)) {
            err = Pa_AbortStream (stream);
            if (err != paNoError) {
                trace("Failed to abort stream. %s\n", Pa_GetErrorText(err))
                // Try to close through callback
                userData->function = STREAM_ABORT;
                trace ("portaudio_stop: aborting stream through callback\n");
                int i = 0;
                while (userData->function != STREAM_FINISHED){
                    usleep (10000);
                    if (i++ > 50) {
                        trace ("portaudio_stop: closing failed\n");
                        state = OUTPUT_STATE_STOPPED;
                        return -1;
                    }
                }
            }
            free (userData);
            userData = 0;
            
            trace ("portaudio_stop: closing stream\n");
            err = Pa_CloseStream (stream);
            stream = 0;
            if (err != paNoError) {
                trace("Failed to close stream. %s\n", Pa_GetErrorText(err))
                state = OUTPUT_STATE_STOPPED;
                return -1;
            }
        }
    }
    state = OUTPUT_STATE_STOPPED;
    deadbeef->streamer_reset (1);
    return 0;
}

static int
portaudio_pause (void) {
    trace ("portaudio_pause\n");
    if (state == OUTPUT_STATE_STOPPED) {
        return -1;
    }
    PaError err;
    err = Pa_AbortStream (stream);
    if (err != paNoError) {
        trace ("Failed to pause stream. %s\n", Pa_GetErrorText(err));
        return -1;
    }
    // set pause state
    state = OUTPUT_STATE_PAUSED;
    return 0;
}

static int
portaudio_unpause (void) {
    trace ("portaudio_unpause\n");
    if (!(state == OUTPUT_STATE_PAUSED)) {
        return -1;
    }
    return portaudio_play ();
}

static int portaudio_get_endiannerequested_fmt (void) {
#if WORDS_BIGENDIAN
    return 1;
#else
    return 0;
#endif
}

// derived from alsa plugin
// derived from alsa-utils/aplay.c
/*static void
palsa_enum_soundcards (void (*callback)(const char *name, const char *desc, void *), void *userdata) {
    void **hints, **n;
    char *name, *descr, *io;
    const char *filter = "Output";
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return;
    n = hints;
    while (*n != NULL) {
        name = snd_device_name_get_hint(*n, "NAME");
        descr = snd_device_name_get_hint(*n, "DESC");
        io = snd_device_name_get_hint(*n, "IOID");
        if (io == NULL || !strcmp(io, filter)) {
            if (name && descr && callback) {
                callback (name, descr, userdata);
            }
        }
        if (name != NULL)
            free(name);
        if (descr != NULL)
            free(descr);
        if (io != NULL)
            free(io);
        n++;
    }
    snd_device_name_free_hint(hints);
}*/

static void portaudio_enum_soundcards (void (*callback)(const char *name, const char *desc, void*), void *userdata) {
    PaDeviceIndex  device_count = Pa_GetDeviceCount ();
    if (!device_count)
        trace("no devices found?\n");

    PaDeviceIndex i = 0;
    trace ("portaudio_enum_soundcards have %d devices\n",device_count);
    for (i=0;i<device_count;i++) {
        const PaDeviceInfo* device = Pa_GetDeviceInfo (i);
        if (!device) {
            trace ("reading device info failed\n");
        }
        
        //char * name_charset = deadbeef->junk_detect_charset (device->name);
        const char *name_converted = device->name;

        // Convert to UTF-8 on windows
        #ifdef __MINGW32__
        /*
        if (name_charset != NULL) {
            trace ( "name using %s charset, converting\n",name_charset);
            name_converted = malloc (strlen(device->name) * 4);
            if (name_converted) {
                deadbeef->junk_iconv (device->name, strlen(device->name), name_converted, strlen(device->name)*4, "cp1250", "UTF-8");
            }
        }
        */
        wchar_t wideName[255];
        int err = 0;
        err = MultiByteToWideChar(CP_UTF8, 0, device->name, -1, wideName, 255);
        char convName[255];
        sprintf(&convName,"%ws", wideName);
        name_converted = convName;
        #endif
        if( device->name && callback)
            callback ("test",name_converted, userdata);
        trace ("device: %s\n",name_converted);
    }

}

static int
portaudio_callback (const void *in, void *out, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *uData ) {
    if (!deadbeef->streamer_ok_to_read (-1)) {
        trace ("portaudio_callback [%d]: wait\n",((struct uData *) uData)->num);
        usleep (20000);
    }
    if ( ((struct uData *) uData)->function == STREAM_COMPLETE) {
        trace ("portaudio_callback [%d]: format changed, aborting stream\n",((struct uData *) uData)->num);
        ((struct uData *) uData)->function = STREAM_FINISHED;
        return paComplete;
        //return paAbort;
    }
    else if ( ((struct uData *) uData)->function == STREAM_ABORT) {
        trace ("portaudio_callback [%d]: aborting\n",((struct uData *) uData)->num);
        ((struct uData *) uData)->function = STREAM_FINISHED;
        
        return paAbort;
    }
    if (((struct uData *) uData)->function == STREAM_MURDER) {
        //trace ("portaudio_callback (new): waiting for old stream to finish\n");
        //int i = 0;
        /*
        while (*userDataOld != STREAM_FINISHED){
            usleep (1000);
            if (i++ > 1000){
                trace ("portaudio_callback (new): timeout\n");
                break;
            }
        }*/
        if (previous_stream) {
            PaError err;
            free (userDataOld);
            userDataOld = 0;
            trace ("portaudio_callback [%d]: closing old stream\n",((struct uData *) uData)->num);
            err = Pa_CloseStream (previous_stream);
            if (err != paNoError) {
                trace("Failed to close stream. %s\n", Pa_GetErrorText(err))
                //return -1;
            }
            previous_stream = 0;
        }
        ((struct uData *) uData)->function = STREAM_CONTINUE;
    }
    if ( ((struct uData *) uData)->function == STREAM_SWITCH_WAIT ) {
        trace ("portaudio_callback [%d]: switch wait\n",((struct uData *) uData)->num);
        int i = 0;
        while (((struct uData *) uData)->function == STREAM_SWITCH_WAIT) {
            usleep (1000);
            if (((struct uData *) uData)->function == STREAM_CONTINUE)
                break;
            if (i++ > 10000){
                trace ("portaudio_callback [%d]: wait timeout\n",((struct uData *) uData)->num);
                break;
            }
        }
        trace ("portaudio_callback [%d]: switch exit after %d loops\n",((struct uData *) uData)->num, i);
        ((struct uData *) uData)->function = STREAM_MURDER;
    }

    if (state != OUTPUT_STATE_PLAYING){
        trace ("portaudio_callback [%d]: abort\n", ((struct uData *) uData)->num);
        return paAbort;
    }
    deadbeef->streamer_read (out, framesPerBuffer*plugin.fmt.channels*plugin.fmt.bps/8);
    return paContinue;
}

static void pa_SetDefault (){
    stream_parameters.device = -1;
    stream_parameters.channelCount = plugin.fmt.channels;
    stream_parameters.sampleFormat = pa_GetSampleFormat (plugin.fmt.bps, plugin.fmt.is_float);
    stream_parameters.suggestedLatency = 0.0;
    stream_parameters.hostApiSpecificStreamInfo = NULL;
}

static int
portaudio_get_state (void) {
    return state;
}

static const char settings_dlg[] =
    "property \"Buffer size (-1 to use optimal value choosen by portaudio)\" entry portaudio.buffer " DEFAULT_BUFFER_SIZE_STR ";\n"
;

static int
p_portaudio_start (void) {
    mutex = deadbeef->mutex_create ();
    PaError err;
    err = Pa_Initialize ();
    if (err != paNoError) {
        trace ("Failed to initialize PortAudio. %s\n", Pa_GetErrorText(err));
        return -1;
    }
    pa_SetDefault ();
    return 0;
}

static int
p_portaudio_stop (void) {
    deadbeef->mutex_free (mutex);
    PaError err;
    err = Pa_Terminate ();
    if (err != paNoError) {
        trace ("Failed to terminate PortAudio. %s\n", Pa_GetErrorText(err));
        return -1;
    }
    return 0;
}

DB_plugin_t *
portaudio_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

static DB_output_t plugin = {
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 10,
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_OUTPUT,
    .plugin.id = "portaudio",
    .plugin.name = "PortAudio output plugin",
    .plugin.descr = "This plugin plays audio using PortAudio library.",
    .plugin.copyright =
    "PortAudio output plugin for DeaDBeeF Player\n"
    "Copyright (C) 2017 Jakub Wasylków\n"
    "\n"
    "This software is provided 'as-is', without any express or implied\n"
    "warranty.  In no event will the authors be held liable for any damages\n"
    "arising from the use of this software.\n"
    "\n"
    "Permission is granted to anyone to use this software for any purpose,\n"
    "including commercial applications, and to alter it and redistribute it\n"
    "freely, subject to the following restrictions:\n"
    "\n"
    "1. The origin of this software must not be misrepresented; you must not\n"
    " claim that you wrote the original software. If you use this software\n"
    " in a product, an acknowledgment in the product documentation would be\n"
    " appreciated but is not required.\n"
    "\n"
    "2. Altered source versions must be plainly marked as such, and must not be\n"
    " misrepresented as being the original software.\n"
    "\n"
    "3. This notice may not be removed or altered from any source distribution.\n"
    ,
    .plugin.website = "http://github.com/kuba160",
    .plugin.start = p_portaudio_start,
    .plugin.stop = p_portaudio_stop,
    .plugin.configdialog = settings_dlg,
    .init = portaudio_init,
    .free = portaudio_free,
    .setformat = portaudio_setformat,
    .play = portaudio_play,
    .stop = portaudio_stop,
    .pause = portaudio_pause,
    .unpause = portaudio_unpause,
    .state = portaudio_get_state,
    .fmt = {.samplerate = 44100, .channels = 2, .bps = 16, .channelmask = DDB_SPEAKER_FRONT_LEFT | DDB_SPEAKER_FRONT_RIGHT},
    .enum_soundcards = portaudio_enum_soundcards
};
