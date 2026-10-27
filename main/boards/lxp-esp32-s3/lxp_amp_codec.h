#pragma once

#include "codecs/no_audio_codec.h"

#include <driver/gpio.h>

/**
 * MAX98357：I2S 时钟常开，SD 常高（曾用 SD 静音导致整段无声）。
 */
class LxpAmpCodec : public NoAudioCodecSimplex {
public:
    LxpAmpCodec(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws,
                gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck,
                gpio_num_t mic_ws, gpio_num_t mic_din, i2s_std_slot_mask_t mic_slot_mask,
                gpio_num_t sd_gpio);

    void Start() override;
    void EnableOutput(bool enable) override;

private:
    gpio_num_t sd_gpio_;
    bool i2s_clocks_on_ = false;
    void SetSd(bool on);
};
