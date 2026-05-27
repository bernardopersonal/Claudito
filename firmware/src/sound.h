#pragma once
#include "sound_data.h"   // sound_id_t, sound_table

/// Initialise the audio HAL and start the background playback task.
/// Call once from setup(), after board_init() / Wire.begin().
void sound_init(void);

/// Queue a sound for playback. Non-blocking; if a sound is already
/// playing the new request is silently dropped (no queue stacking).
void sound_play(sound_id_t id);

/// Set master volume (0..100). Persists until changed.
void sound_set_volume(uint8_t vol);
