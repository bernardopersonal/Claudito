#include "sound.h"
#include "hal/audio_hal.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <pgmspace.h>

static QueueHandle_t sound_queue = nullptr;
static uint8_t master_vol = 75;

// Background task: waits for a sound_id on the queue, streams it to the DAC.
static void sound_task(void* arg) {
    sound_id_t id;
    for (;;) {
        if (xQueueReceive(sound_queue, &id, portMAX_DELAY) == pdTRUE) {
            if (id >= SND_COUNT) continue;
            const SoundEntry& entry = sound_table[id];
            const int16_t* src = entry.data;
            size_t remaining = entry.length;

            // Stream in 128-sample chunks. pgm_read_word fetches from flash
            // on AVR; on ESP32 it's a no-op (flash is memory-mapped), but
            // we keep it for portability.
            static const size_t CHUNK = 128;
            int16_t buf[CHUNK];
            while (remaining > 0) {
                size_t n = remaining > CHUNK ? CHUNK : remaining;
                for (size_t i = 0; i < n; i++) {
                    buf[i] = (int16_t)pgm_read_word(&src[i]);
                }
                audio_hal_write(buf, n);
                src += n;
                remaining -= n;
            }

            // Flush a small silence tail so the DMA completes the last chunk.
            int16_t zeros[64] = {0};
            audio_hal_write(zeros, 64);
        }
    }
}

void sound_init(void) {
    if (!audio_hal_init()) {
        Serial.println("Sound: audio HAL init failed — sounds disabled");
        return;
    }
    audio_hal_set_volume(master_vol);

    sound_queue = xQueueCreate(2, sizeof(sound_id_t));
    if (!sound_queue) {
        Serial.println("Sound: queue create failed");
        return;
    }

    // Stack: ~1.5 KB for the task + 128*2 buf + 64*4 stereo in audio_hal_write.
    // 3072 bytes should be plenty.
    xTaskCreate(sound_task, "snd", 3072, nullptr, 1, nullptr);
    Serial.println("Sound: init OK");
}

void sound_play(sound_id_t id) {
    if (!sound_queue || id >= SND_COUNT) return;
    // Non-blocking enqueue; if full (sound already playing) silently drop.
    xQueueSend(sound_queue, &id, 0);
}

void sound_set_volume(uint8_t vol) {
    if (vol > 100) vol = 100;
    master_vol = vol;
    audio_hal_set_volume(vol);
}
