/*
 * audio.h — sound-effect playback through SDL audio.
 *
 * A small registry of decoded WAV clips, each bound to its own logical device
 * stream on the system's default playback device so concurrent plays mix.
 * Register during setup (like the other def registrations); play any-thread.
 * When no playback device exists (headless CI) registration still validates
 * and returns ids, and playback is a silent no-op.
 */
#ifndef TESSERA_AUDIO_H
#define TESSERA_AUDIO_H

#include "core/core.h"
#include "tessera.h"

typedef struct TsAudio TsAudio;

TsAudio* ts_audio_create(const TsLog* log);
void     ts_audio_destroy(TsAudio* a);

/* Decode `wav` (file bytes or a path) and return its sound id (0 = failure,
 * message in err). Setup-phase only, like the registry adds. */
uint32_t ts_audio_register(TsAudio* a, const TesseraBytes* wav,
                           char* err, size_t err_sz);

/* Restart clip `id` at `gain` (1 = as authored; clamped at 0). Any-thread.
 * False when the id is unknown or there is no playback device. */
bool ts_audio_play(TsAudio* a, uint32_t id, float gain);

#endif /* TESSERA_AUDIO_H */
