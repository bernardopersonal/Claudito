// S3 AMOLED-2.16: no ES8311 codec — audio_hal stubs.
#include "../../hal/audio_hal.h"

bool   audio_hal_init(void)                                   { return false; }
size_t audio_hal_write(const int16_t*, size_t)                { return 0; }
void   audio_hal_set_volume(uint8_t)                          {}
void   audio_hal_mute(bool)                                   {}
