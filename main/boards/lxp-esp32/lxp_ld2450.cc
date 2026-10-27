#include "lxp_ld2450.h"
#include "config.h"

#include <climits>
#include <cmath>
#include <cstring>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#define TAG "LxpLd2450"

namespace {
constexpr uint8_t kHdr0 = 0xAA;
constexpr uint8_t kHdr1 = 0xFF;
constexpr uint8_t kHdr2 = 0x03;
constexpr uint8_t kHdr3 = 0x00;
constexpr uint8_t kFtr0 = 0x55;
constexpr uint8_t kFtr1 = 0xCC;

/** LD2450 encodes sign in MSB with absolute value in lower 15 bits. */
int16_t DecodeCoord(const uint8_t* p) {
    uint16_t raw = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (raw & 0x8000) {
        return -(int16_t)(raw & 0x7FFF);
    }
    return (int16_t)(raw & 0x7FFF);
}

int16_t DecodeSpeed(const uint8_t* p) {
    return DecodeCoord(p);
}

// Diagnostics (reset each Start)
uint32_t g_rx_bytes = 0;
uint32_t g_frames_ok = 0;
uint32_t g_frames_bad = 0;
}  // namespace

LxpLd2450& LxpLd2450::GetInstance() {
    static LxpLd2450 instance;
    return instance;
}

bool LxpLd2450::Start() {
#if !LXP_LD2450_ENABLE
    ESP_LOGW(TAG, "LD2450 disabled in config");
    return false;
#else
    if (started_) {
        return true;
    }

    uart_config_t cfg = {};
    cfg.baud_rate = LXP_LD2450_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(LXP_LD2450_UART_NUM, kRxBuf * 2, 0, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(LXP_LD2450_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
        return false;
    }

    // RX-only: TX = NC / -1 so UART0 console TX can still work on GPIO1
    int tx = (LXP_LD2450_TX_GPIO == GPIO_NUM_NC) ? UART_PIN_NO_CHANGE : (int)LXP_LD2450_TX_GPIO;
    err = uart_set_pin(LXP_LD2450_UART_NUM, tx, (int)LXP_LD2450_RX_GPIO, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin: %s", esp_err_to_name(err));
        return false;
    }

    uart_flush_input(LXP_LD2450_UART_NUM);
    g_rx_bytes = g_frames_ok = g_frames_bad = 0;
    started_ = true;
    ESP_LOGI(TAG, "LD2450 UART%d RX=GPIO%d baud=%d (TX unused). Wire: module TX -> board RX2",
             (int)LXP_LD2450_UART_NUM, (int)LXP_LD2450_RX_GPIO, LXP_LD2450_BAUD);
    return true;
#endif
}

void LxpLd2450::Feed(const uint8_t* data, int len) {
    if (len <= 0) {
        return;
    }
    if (accum_len_ + len > kRxBuf) {
        // Keep last window; drop oldest
        int keep = kRxBuf / 2;
        if (accum_len_ > keep) {
            memmove(accum_, accum_ + (accum_len_ - keep), keep);
            accum_len_ = keep;
        } else {
            accum_len_ = 0;
        }
    }
    int copy = len;
    if (accum_len_ + copy > kRxBuf) {
        copy = kRxBuf - accum_len_;
    }
    if (copy > 0) {
        memcpy(accum_ + accum_len_, data, copy);
        accum_len_ += copy;
    }
    while (TryParseOneFrame()) {
    }
}

bool LxpLd2450::TryParseOneFrame() {
    // Find header AA FF 03 00
    int start = -1;
    for (int i = 0; i + 3 < accum_len_; ++i) {
        if (accum_[i] == kHdr0 && accum_[i + 1] == kHdr1 && accum_[i + 2] == kHdr2 &&
            accum_[i + 3] == kHdr3) {
            start = i;
            break;
        }
    }
    if (start < 0) {
        // Keep last 3 bytes in case header spans chunks
        if (accum_len_ > 3) {
            memmove(accum_, accum_ + (accum_len_ - 3), 3);
            accum_len_ = 3;
        }
        return false;
    }
    if (start > 0) {
        memmove(accum_, accum_ + start, accum_len_ - start);
        accum_len_ -= start;
    }
    if (accum_len_ < kFrameLen) {
        return false;
    }
    if (accum_[kFrameLen - 2] != kFtr0 || accum_[kFrameLen - 1] != kFtr1) {
        // Bad footer — skip one byte and resync
        g_frames_bad++;
        memmove(accum_, accum_ + 1, accum_len_ - 1);
        accum_len_ -= 1;
        return true;  // keep trying
    }

    // 3 targets × 8 bytes after 4-byte header
    int best_mm = INT32_MAX;
    int best_speed = 0;
    int best_x = 0;
    int best_y = 0;
    bool any = false;

    for (int t = 0; t < 3; ++t) {
        const uint8_t* p = accum_ + 4 + t * 8;
        int16_t x = DecodeCoord(p);
        int16_t y = DecodeCoord(p + 2);
        int16_t speed = DecodeSpeed(p + 4);
        // Empty slot: all zeros (ignore distance resolution nibble)
        if (x == 0 && y == 0 && speed == 0) {
            continue;
        }
        // Accept any orientation: some mounts flip Y sign (was: y<=0 skip → 测不到人)
        int dist = (int)lroundf(hypotf((float)x, (float)y));
        if (dist <= 0) {
            dist = abs((int)y) > 0 ? abs((int)y) : abs((int)x);
        }
        if (dist < best_mm) {
            best_mm = dist;
            best_speed = speed;
            best_x = x;
            best_y = y;
            any = true;
        }
    }

    g_frames_ok++;
    if (any) {
        last_.present = true;
        last_.nearest_mm = best_mm;
        last_.speed_mm_s = best_speed;
        last_.x_mm = best_x;
        last_.y_mm = best_y;
        last_present_us_ = esp_timer_get_time();
    }

    memmove(accum_, accum_ + kFrameLen, accum_len_ - kFrameLen);
    accum_len_ -= kFrameLen;
    return true;
}

LxpLd2450::Reading LxpLd2450::Poll() {
    Reading out{};
#if !LXP_LD2450_ENABLE
    return out;
#else
    if (!started_) {
        return out;
    }

    uint8_t tmp[128];
    while (true) {
        int n = uart_read_bytes(LXP_LD2450_UART_NUM, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            break;
        }
        g_rx_bytes += (uint32_t)n;
        Feed(tmp, n);
    }

    int64_t now = esp_timer_get_time();
    out = last_;
    // Soft hold 1.5s（原 300ms 过短，静止目标帧间隙会被判无人）
    constexpr int64_t kHoldUs = 1500 * 1000LL;
    if (last_present_us_ > 0 && (now - last_present_us_) <= kHoldUs) {
        out.present = true;
    } else {
        out.present = false;
    }
    return out;
#endif
}

/** Expose diagnostics for periodic board logs / MCP. */
void LxpLd2450::GetDiag(uint32_t* rx_bytes, uint32_t* frames_ok, uint32_t* frames_bad) {
    if (rx_bytes) *rx_bytes = g_rx_bytes;
    if (frames_ok) *frames_ok = g_frames_ok;
    if (frames_bad) *frames_bad = g_frames_bad;
}
