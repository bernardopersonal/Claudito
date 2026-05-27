#include "../../hal/audio_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>

// ---- ES8311 I2C address and key registers ----
#define ES8311_ADDR             0x18

#define ES8311_REG00_RESET      0x00
#define ES8311_REG01_CLK_MGR    0x01
#define ES8311_REG02_CLK_MGR    0x02
#define ES8311_REG03_CLK_MGR    0x03
#define ES8311_REG04_CLK_MGR    0x04
#define ES8311_REG05_CLK_MGR    0x05
#define ES8311_REG06_CLK_MGR    0x06
#define ES8311_REG09_SDP_IN     0x09   // DAC serial data port format
#define ES8311_REG0A_SDP_OUT    0x0A   // ADC serial data port format
#define ES8311_REG0B_SYS        0x0B
#define ES8311_REG0C_SYS        0x0C
#define ES8311_REG0D_SYS        0x0D   // power up analog
#define ES8311_REG0E_SYS        0x0E   // enable PGA + ADC
#define ES8311_REG10_SYS        0x10
#define ES8311_REG11_SYS        0x11
#define ES8311_REG12_SYS        0x12   // DAC power
#define ES8311_REG13_SYS        0x13   // output enable
#define ES8311_REG14_SYS        0x14
#define ES8311_REG15_ADC        0x15
#define ES8311_REG16_ADC        0x16
#define ES8311_REG17_ADC        0x17
#define ES8311_REG1B_ADC        0x1B
#define ES8311_REG1C_ADC        0x1C
#define ES8311_REG32_DAC_VOL    0x32   // DAC volume (0x00..0xFF)
#define ES8311_REG37_DAC        0x37
#define ES8311_REG44_GPIO       0x44
#define ES8311_REG45_GP         0x45

// ---- I2S pins for this board ----
#define I2S_MCLK_PIN    GPIO_NUM_19
#define I2S_BCLK_PIN    GPIO_NUM_20
#define I2S_WS_PIN      GPIO_NUM_22
#define I2S_DOUT_PIN    GPIO_NUM_23   // ESP TX → ES8311 SDIN (speaker)
#define I2S_DIN_PIN     GPIO_NUM_21   // ESP RX ← ES8311 SDOUT (mic, unused)

// Use the sample rate from the generated sound data header.
#include "../../sound_data.h"
#define AUDIO_SAMPLE_RATE   SOUND_SAMPLE_RATE

static i2s_chan_handle_t tx_handle = nullptr;

// ---- ES8311 I2C helpers ----

static bool es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static uint8_t es_read(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(ES8311_ADDR, (uint8_t)1);
    return Wire.read();
}

// ---- ES8311 init sequence (from Espressif es8311.c, slave mode, MCLK on) ----

static bool es8311_init() {
    // Probe: write twice to GPIO reg (I2C noise immunity quirk)
    if (!es_write(ES8311_REG44_GPIO, 0x08)) {
        Serial.println("ES8311: not found at 0x18");
        return false;
    }
    es_write(ES8311_REG44_GPIO, 0x08);

    // Clock manager defaults
    es_write(ES8311_REG01_CLK_MGR, 0x30);
    es_write(ES8311_REG02_CLK_MGR, 0x00);
    es_write(ES8311_REG03_CLK_MGR, 0x10);
    es_write(ES8311_REG16_ADC,     0x24);
    es_write(ES8311_REG04_CLK_MGR, 0x10);
    es_write(ES8311_REG05_CLK_MGR, 0x00);

    // System registers
    es_write(ES8311_REG0B_SYS, 0x00);
    es_write(ES8311_REG0C_SYS, 0x00);
    es_write(ES8311_REG10_SYS, 0x1F);
    es_write(ES8311_REG11_SYS, 0x7F);

    // CSM_ON — start state machine, slave mode, use MCLK
    uint8_t reg00 = 0x80;  // CSM power up
    // Slave mode: bit 6 = 0
    es_write(ES8311_REG00_RESET, reg00);

    // CLK_MGR_REG01: use_mclk=1 (bit7=0), no invert
    es_write(ES8311_REG01_CLK_MGR, 0x3F);

    // Output enable, ADC config
    es_write(ES8311_REG13_SYS, 0x10);
    es_write(ES8311_REG1B_ADC, 0x0A);
    es_write(ES8311_REG1C_ADC, 0x6A);

    // GPIO: enable DAC reference
    es_write(ES8311_REG44_GPIO, 0x58);

    // ---- Start DAC path ----
    // SDP IN (DAC): unmute
    uint8_t dac_iface = es_read(ES8311_REG09_SDP_IN);
    dac_iface &= 0xBF;  // clear mute bit
    es_write(ES8311_REG09_SDP_IN, dac_iface);

    // Configure 16-bit I2S format for DAC input
    // Bits [4:2] = word length: 011 = 16-bit
    uint8_t sdp = es_read(ES8311_REG09_SDP_IN);
    sdp &= ~(0x1C);        // clear bits [4:2]
    sdp |= (0x03 << 2);    // 16-bit
    es_write(ES8311_REG09_SDP_IN, sdp);

    // ADC volume
    es_write(ES8311_REG17_ADC, 0xBF);
    // Power up analog: VMIDSEL + reference
    es_write(ES8311_REG0E_SYS, 0x02);
    // Power up DAC
    es_write(ES8311_REG12_SYS, 0x00);
    // Enable DAC output stage
    es_write(ES8311_REG14_SYS, 0x1A);
    // Power up analog bias
    es_write(ES8311_REG0D_SYS, 0x01);
    // ADC input gain
    es_write(ES8311_REG15_ADC, 0x40);
    // DAC EQ bypass
    es_write(ES8311_REG37_DAC, 0x08);
    // GP register
    es_write(ES8311_REG45_GP, 0x00);

    // ---- Configure sample rate ----
    // I2S master (ESP32) provides MCLK = 256 × fs.
    // REG02[7:5] = pre-divider, REG02[4:0] = MCLK coefficient select.
    // Default values (0x00, 0x10, 0x10) work for common rates when the I2S
    // driver derives MCLK automatically from the configured sample rate.
    es_write(ES8311_REG02_CLK_MGR, 0x00);
    es_write(ES8311_REG03_CLK_MGR, 0x10);  // ADC OSR
    es_write(ES8311_REG04_CLK_MGR, 0x10);  // DAC OSR

    // Set initial volume to ~70%
    es_write(ES8311_REG32_DAC_VOL, 0xBF);

    Serial.println("ES8311: init OK");
    return true;
}

// ---- I2S init ----

static bool i2s_init() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 256;

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
        Serial.printf("I2S: channel create failed (%d)\n", err);
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("I2S: init failed (%d)\n", err);
        return false;
    }

    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
        Serial.printf("I2S: enable failed (%d)\n", err);
        return false;
    }

    Serial.printf("I2S: init OK (%d Hz, 16-bit stereo)\n", AUDIO_SAMPLE_RATE);
    return true;
}

// ---- Public API ----

bool audio_hal_init(void) {
    if (!es8311_init()) return false;
    if (!i2s_init()) return false;
    return true;
}

size_t audio_hal_write(const int16_t* mono_samples, size_t count) {
    if (!tx_handle || !mono_samples || count == 0) return 0;

    // Convert mono to stereo: duplicate each sample to L+R.
    // Process in small chunks to limit stack usage.
    static const size_t CHUNK = 64;                    // mono samples per chunk
    int16_t stereo_buf[CHUNK * 2];                     // L,R,L,R,...

    size_t written = 0;
    while (written < count) {
        size_t n = count - written;
        if (n > CHUNK) n = CHUNK;

        for (size_t i = 0; i < n; i++) {
            int16_t s = mono_samples[written + i];
            stereo_buf[i * 2]     = s;   // left
            stereo_buf[i * 2 + 1] = s;   // right
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(
            tx_handle,
            stereo_buf,
            n * 4,           // stereo 16-bit = 4 bytes per sample
            &bytes_written,
            pdMS_TO_TICKS(100)
        );
        if (err != ESP_OK) break;
        written += bytes_written / 4;
    }
    return written;
}

void audio_hal_set_volume(uint8_t vol) {
    // Map 0..100 to ES8311 DAC volume register (0x00 = -95.5 dB, 0xFF = +32 dB).
    // Usable range: ~0xA0 (-15 dB) to 0xCF (0 dB). We map 0..100 to 0x00..0xCF.
    uint8_t reg = (uint8_t)((uint32_t)vol * 0xCF / 100);
    es_write(ES8311_REG32_DAC_VOL, reg);
}

void audio_hal_mute(bool mute) {
    uint8_t reg = es_read(ES8311_REG09_SDP_IN);
    if (mute) reg |= 0x40;    // set mute bit
    else      reg &= ~0x40;   // clear mute bit
    es_write(ES8311_REG09_SDP_IN, reg);
}
