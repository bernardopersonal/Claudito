#pragma once
#include <stdint.h>
#include <stddef.h>

/// Initialise ES8311 codec over I2C and configure I2S output.
/// Call after Wire.begin() (board_init) and before sound_init().
bool audio_hal_init(void);

/// Write mono 16-bit PCM samples to the DAC (blocks until DMA accepts them).
/// Samples are duplicated to both stereo channels internally.
/// Returns the number of mono samples actually written.
size_t audio_hal_write(const int16_t* mono_samples, size_t count);

/// Set DAC output volume.  0 = mute, 100 = max.
void audio_hal_set_volume(uint8_t vol);

/// Mute / unmute the DAC without changing the volume register.
void audio_hal_mute(bool mute);
