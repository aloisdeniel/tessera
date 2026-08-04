/* audio.c — WAV sound effects on SDL audio device streams. */
#include "audio/audio.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SDL_AudioSpec    spec;
    Uint8*           buf;    /* SDL-allocated by SDL_LoadWAV_IO */
    Uint32           len;
    SDL_AudioStream* stream; /* owns its logical device; NULL when headless */
} TsSound;

struct TsAudio {
    const TsLog* log;
    bool         device_ok;  /* audio subsystem initialized */
    TsSound*     sounds;
    uint32_t     count, cap;
};

TsAudio* ts_audio_create(const TsLog* log) {
    TsAudio* a = (TsAudio*)calloc(1, sizeof(TsAudio));
    if (!a) return NULL;
    a->log = log;
    a->device_ok = SDL_WasInit(SDL_INIT_AUDIO) || SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!a->device_ok)
        TS_LOGW(log, "audio: SDL audio unavailable (%s) — sounds are silent",
                SDL_GetError());
    return a;
}

void ts_audio_destroy(TsAudio* a) {
    if (!a) return;
    for (uint32_t i = 0; i < a->count; ++i) {
        if (a->sounds[i].stream) SDL_DestroyAudioStream(a->sounds[i].stream);
        if (a->sounds[i].buf) SDL_free(a->sounds[i].buf);
    }
    free(a->sounds);
    free(a);
}

uint32_t ts_audio_register(TsAudio* a, const TesseraBytes* wav,
                           char* err, size_t err_sz) {
    if (!a || !wav) return 0;
    SDL_IOStream* io = NULL;
    if (wav->data && wav->size) {
        io = SDL_IOFromConstMem(wav->data, wav->size);
    } else if (wav->path) {
        io = SDL_IOFromFile(wav->path, "rb");
    }
    if (!io) {
        snprintf(err, err_sz, "register_sound(%s): no bytes (%s)",
                 wav->debug_name ? wav->debug_name : "?", SDL_GetError());
        return 0;
    }

    TsSound s;
    memset(&s, 0, sizeof s);
    if (!SDL_LoadWAV_IO(io, true, &s.spec, &s.buf, &s.len)) {
        snprintf(err, err_sz, "register_sound(%s): %s",
                 wav->debug_name ? wav->debug_name : "?", SDL_GetError());
        return 0;
    }

    /* A stream per clip on the default device: concurrent clips mix, and a
     * retrigger simply restarts its own stream. Headless keeps stream NULL. */
    if (a->device_ok) {
        s.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                             &s.spec, NULL, NULL);
        if (s.stream) {
            SDL_ResumeAudioStreamDevice(s.stream);
        } else {
            TS_LOGW(a->log, "audio: no playback stream (%s) — sound %u silent",
                    SDL_GetError(), a->count + 1);
        }
    }

    if (a->count == a->cap) {
        uint32_t cap = a->cap ? a->cap * 2 : 8;
        TsSound* grown = (TsSound*)realloc(a->sounds, cap * sizeof(TsSound));
        if (!grown) {
            if (s.stream) SDL_DestroyAudioStream(s.stream);
            SDL_free(s.buf);
            snprintf(err, err_sz, "register_sound: out of memory");
            return 0;
        }
        a->sounds = grown;
        a->cap = cap;
    }
    a->sounds[a->count] = s;
    return ++a->count;   /* ids are index+1, first id is 1 */
}

uint32_t ts_audio_sound_count(const TsAudio* a) {
    return a ? a->count : 0;
}

bool ts_audio_play(TsAudio* a, uint32_t id, float gain) {
    if (!a || id == 0 || id > a->count) return false;
    TsSound* s = &a->sounds[id - 1];
    if (!s->stream) return false;
    SDL_ClearAudioStream(s->stream);
    SDL_SetAudioStreamGain(s->stream, gain < 0 ? 0 : gain);
    return SDL_PutAudioStreamData(s->stream, s->buf, (int)s->len);
}
