#pragma once

#include "audio_codec.h"

#include <driver/gpio.h>
#include <mutex>

/** MAX98357 清晰度优化：16-bit I2S + 轻微高频提升，避免 32-bit/重采样发闷 */
class LxpMax98357Codec : public AudioCodec {
public:
    LxpMax98357Codec(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk,
                     gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws,
                     gpio_num_t mic_din);
    ~LxpMax98357Codec() override;

    void Start() override;

protected:
    int Write(const int16_t* data, int samples) override;
    int Read(int16_t* dest, int samples) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;

private:
    std::mutex data_if_mutex_;
};
