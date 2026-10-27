#include "lxp_amp_codec.h"
#include "config.h"

#include <driver/i2s_common.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "LxpAmp"

LxpAmpCodec::LxpAmpCodec(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk,
                         gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask,
                         gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din,
                         i2s_std_slot_mask_t mic_slot_mask, gpio_num_t sd_gpio)
    : NoAudioCodecSimplex(input_sample_rate, output_sample_rate, spk_bclk, spk_ws, spk_dout,
                          spk_slot_mask, mic_sck, mic_ws, mic_din, mic_slot_mask),
      sd_gpio_(sd_gpio) {
    // 曾经用 SD 静音导致整段无声；先常开功放（与「有声音了」时一致）
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << sd_gpio_,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(sd_gpio_, 1);
    ESP_LOGI(TAG, "MAX98357 SD GPIO%d forced HIGH (always on)", (int)sd_gpio_);
}

void LxpAmpCodec::SetSd(bool on) { gpio_set_level(sd_gpio_, on ? 1 : 0); }

void LxpAmpCodec::Start() {
    AudioCodec::Start();
    // 防破音，但别压太低导致听不清
    if (output_volume() > 75) {
        ESP_LOGW(TAG, "Clamp volume %d → 70", output_volume());
        SetOutputVolume(70);
    } else if (output_volume() < 40) {
        ESP_LOGW(TAG, "Raise volume %d → 60 (was too quiet)", output_volume());
        SetOutputVolume(60);
    }
    SetSd(true);
}

void LxpAmpCodec::EnableOutput(bool enable) {
    if (enable == output_enabled()) {
        return;
    }
    if (enable) {
        if (!i2s_clocks_on_) {
            ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
            i2s_clocks_on_ = true;
            vTaskDelay(pdMS_TO_TICKS(30));
            ESP_LOGI(TAG, "I2S TX clocks started (stay on)");
        }
        SetSd(true);
        AudioCodec::EnableOutput(true);
        ESP_LOGI(TAG, "Amp output enable (SD stays HIGH)");
    } else {
        // 不拉低 SD：只标记软件静音；I2S 时钟保持，避免再启停杂音/无声
        AudioCodec::EnableOutput(false);
        ESP_LOGI(TAG, "Amp soft-mute (SD kept HIGH, I2S kept)");
    }
}
