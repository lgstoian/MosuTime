/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 *
 * ES8311 + NS4150B speaker path.  The clips live on the SD card, one small
 * WAV per spoken fragment; nothing is embedded in flash.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mosu_board.h"
#include "mosu_board_priv.h"

static const char *TAG = "mosu_audio";

#define AUDIO_I2S_PORT I2S_NUM_0
#define AUDIO_SAMPLE_RATE 22050

static i2s_chan_handle_t s_tx;
static const audio_codec_data_if_t *s_data_if;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_gpio_if_t *s_gpio_if;
static const audio_codec_if_t *s_codec_if;
static esp_codec_dev_handle_t s_codec;
static bool s_channel_enabled;

static esp_err_t wav_read_header(FILE *file, uint32_t *sample_rate,
                                 uint16_t *channels, uint16_t *bits,
                                 uint32_t *data_size)
{
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool have_fmt = false;

    for (;;) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t size = (uint32_t)chunk[4] | ((uint32_t)chunk[5] << 8) |
                        ((uint32_t)chunk[6] << 16) | ((uint32_t)chunk[7] << 24);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            size_t to_read = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, to_read, file) != to_read) {
                return ESP_ERR_INVALID_SIZE;
            }
            if (size > to_read) {
                fseek(file, (long)(size - to_read), SEEK_CUR);
            }
            uint16_t format = (uint16_t)(fmt[0] | (fmt[1] << 8));
            *channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
            *sample_rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                           ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            *bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
            if (format != 1 /* PCM */) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            *data_size = size;
            return have_fmt ? ESP_OK : ESP_ERR_INVALID_ARG;
        } else {
            fseek(file, (long)size + (long)(size & 1), SEEK_CUR);
        }
    }
}

esp_err_t mosu_board_audio_init(void)
{
    if (s_codec) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG,
                        "i2s channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = MOSU_PIN_I2S_MCLK,
            .bclk = MOSU_PIN_I2S_BCLK,
            .ws = MOSU_PIN_I2S_LRCK,
            .dout = MOSU_PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG,
                        "i2s std");
    /* The channel is enabled before opening the codec stream because the
     * IDF reconfig functions expect a running channel.  esp_codec_dev
     * disables it again in close(); we only track that fact and must not
     * disable it a second time. */
    s_channel_enabled = false;

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = NULL,
        .tx_handle = s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!s_data_if) {
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    s_gpio_if = gpio_if;
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = mosu_board_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    s_ctrl_if = ctrl_if;
    if (!ctrl_if) {
        return ESP_FAIL;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = MOSU_PIN_PA_ENABLE,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };

    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    s_codec_if = codec_if;
    if (!codec_if) {
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = s_data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) {
        return ESP_FAIL;
    }

    /* The vendor driver writes 0xFA here, clearing ES8311 REG0D bit2
     * (PDN_VREF).  Owners of this board measured ~12 mA of extra sleep current
     * with that bit clear; the power-on value 0xFC keeps VREF down properly.
     * Restore it after every init, since we tear the codec down between
     * announcements. */
    uint8_t vref = 0xFC;
    if (s_ctrl_if && s_ctrl_if->write_reg) {
        int patch = s_ctrl_if->write_reg(s_ctrl_if, 0x0D, 1, &vref, 1);
        if (patch != 0) {
            ESP_LOGW(TAG, "ES8311 VREF patch: %d", patch);
        }
    }

    ESP_LOGI(TAG, "audio ready");
    return ESP_OK;
}

static esp_err_t ensure_channel(void)
{
    if (!s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_channel_enabled) {
        return ESP_OK;
    }
    esp_err_t err = i2s_channel_enable(s_tx);
    if (err == ESP_OK) {
        s_channel_enabled = true;
    }
    return err;
}

static esp_err_t open_stream(uint32_t sample_rate)
{
    ESP_RETURN_ON_ERROR(ensure_channel(), TAG, "i2s enable");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = 1,
        .bits_per_sample = 16,
        .mclk_multiple = 0,
    };
    return esp_codec_dev_open(s_codec, &fs);
}

/* The stream must already be open; both helpers are shared by beep() and the
 * console's audio test so the tone path has a single implementation. */
static void write_tone(uint16_t hz, uint16_t ms)
{
    int16_t buffer[512];
    const int total = (AUDIO_SAMPLE_RATE * ms) / 1000;
    int done = 0;
    float phase = 0.0f;
    const float step =
        (2.0f * 3.14159265f * (float)hz) / (float)AUDIO_SAMPLE_RATE;

    while (done < total) {
        int n = total - done;
        if (n > (int)(sizeof(buffer) / sizeof(buffer[0]))) {
            n = (int)(sizeof(buffer) / sizeof(buffer[0]));
        }
        for (int i = 0; i < n; i++) {
            buffer[i] = (int16_t)(sinf(phase) * 8000.0f);
            phase += step;
            if (phase > 6.2831853f) {
                phase -= 6.2831853f;
            }
        }
        esp_codec_dev_write(s_codec, buffer, n * (int)sizeof(int16_t));
        done += n;
    }
}

static void write_silence(uint16_t ms)
{
    int16_t buffer[256] = {0};
    int total = (AUDIO_SAMPLE_RATE * ms) / 1000;

    while (total > 0) {
        int n = total < (int)(sizeof(buffer) / sizeof(buffer[0]))
                    ? total
                    : (int)(sizeof(buffer) / sizeof(buffer[0]));
        esp_codec_dev_write(s_codec, buffer, n * (int)sizeof(int16_t));
        total -= n;
    }
}

esp_err_t mosu_board_audio_idle(void)
{
    if (!s_codec) {
        return ESP_OK;
    }
    esp_codec_dev_close(s_codec);
    s_channel_enabled = false;

    /* An initialised I2S channel prevents light sleep on ESP32-S3 even when
     * it is idle, and an idle ES8311 costs tens of milliamps on this board.
     * Tear the whole path down; mosu_board_audio_init() rebuilds it on the
     * next announcement. */
    esp_codec_dev_delete(s_codec);
    s_codec = NULL;
    if (s_codec_if) {
        audio_codec_delete_codec_if(s_codec_if);
        s_codec_if = NULL;
    }
    if (s_ctrl_if) {
        audio_codec_delete_ctrl_if(s_ctrl_if);
        s_ctrl_if = NULL;
    }
    if (s_data_if) {
        audio_codec_delete_data_if(s_data_if);
        s_data_if = NULL;
    }
    if (s_gpio_if) {
        audio_codec_delete_gpio_if(s_gpio_if);
        s_gpio_if = NULL;
    }
    if (s_tx) {
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    return ESP_OK;
}

esp_err_t mosu_board_audio_play_wav(const char *path, uint8_t volume)
{
    if (!s_codec) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGW(TAG, "missing clip: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t data_size = 0;

    esp_err_t err = wav_read_header(file, &sample_rate, &channels, &bits,
                                    &data_size);
    if (err != ESP_OK) {
        fclose(file);
        ESP_LOGW(TAG, "unsupported wav: %s", path);
        return err;
    }
    if (channels != 1 || bits != 16 || sample_rate < 8000 ||
        sample_rate > 48000) {
        fclose(file);
        ESP_LOGW(TAG, "need mono 16 bit wav, got %u Hz %u ch %u bit: %s",
                 (unsigned)sample_rate, channels, bits, path);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_codec_dev_set_out_vol(s_codec, volume);
    err = open_stream(sample_rate);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }

    uint8_t buffer[1024];
    uint32_t remaining = data_size;
    while (remaining > 0) {
        size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t got = fread(buffer, 1, want, file);
        if (got == 0) {
            break;
        }
        if (esp_codec_dev_write(s_codec, buffer, (int)got) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        remaining -= (uint32_t)got;
    }

    esp_codec_dev_close(s_codec);
    s_channel_enabled = false;
    fclose(file);
    return err;
}

esp_err_t mosu_board_audio_beep(uint16_t hz, uint16_t ms)
{
    if (!s_codec || hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = open_stream(AUDIO_SAMPLE_RATE);
    if (err != ESP_OK) {
        return err;
    }

    write_tone(hz, ms);
    return mosu_board_audio_idle();
}

esp_err_t mosu_board_audio_test(void)
{
    if (!s_codec) {
        return ESP_ERR_INVALID_STATE;
    }

    /* A fixed, audible volume: the test must be heard even when the config
     * volume is zero or the wearer muted the announcements. */
    esp_codec_dev_set_out_vol(s_codec, 70);

    esp_err_t err = open_stream(AUDIO_SAMPLE_RATE);
    if (err != ESP_OK) {
        return err;
    }

    static const uint16_t tones[] = {440, 880, 1320};
    for (size_t i = 0; i < sizeof(tones) / sizeof(tones[0]); i++) {
        if (i > 0) {
            write_silence(60);
        }
        write_tone(tones[i], 120);
    }

    return mosu_board_audio_idle();
}
