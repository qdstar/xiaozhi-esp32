#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "display/display.h"
#include "led/circular_strip.h"
#include "lxp_ld2450.h"
#include "lxp_order_bridge.h"
#include "lxp_shop_claim.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdio>

#include <cctype>
#include <string>

#define TAG "LxpEsp32"

// SoftAP / 展示名：小二；端侧唤醒「你好小二」（S3+PSRAM 开自定义 WN；经典 ESP32 默认 WAKE_WORD_DISABLED，靠 BOOT）
static constexpr const char* kWakeWordDisplay = "你好小二";

namespace {

std::string MacHexUpperNoColon() {
    std::string mac = SystemInfo::GetMacAddress();
    std::string hex;
    hex.reserve(12);
    for (char c : mac) {
        if (c == ':') {
            continue;
        }
        hex.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return hex;
}

struct ActivationReportArgs {
    std::string code;
    std::string message;
};

void ActivationReportTask(void* param) {
    auto* args = static_cast<ActivationReportArgs*>(param);
    auto cleanup = [&]() {
        delete args;
        vTaskDelete(nullptr);
    };

    auto* network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGW(TAG, "activation-code report: network unavailable");
        cleanup();
        return;
    }

    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGW(TAG, "activation-code report: CreateHttp failed");
        cleanup();
        return;
    }

    std::string mac_hex = MacHexUpperNoColon();
    std::string mac_colons = SystemInfo::GetMacAddress();
    std::string pairing;
    {
        Settings s("lxp", false);
        pairing = s.GetString("pending_pairing");
    }

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "code", args->code.c_str());
    cJSON_AddStringToObject(body, "serialNo", mac_hex.c_str());
    cJSON_AddStringToObject(body, "mac", mac_colons.c_str());
    if (!pairing.empty()) {
        cJSON_AddStringToObject(body, "pairingCode", pairing.c_str());
    }
    if (!args->message.empty()) {
        cJSON_AddStringToObject(body, "message", args->message.c_str());
    }
    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) {
        ESP_LOGE(TAG, "activation-code report: JSON build failed");
        cleanup();
        return;
    }

    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetTimeout(15000);
    http->SetContent(std::string(body_str));
    cJSON_free(body_str);

    const char* url = "https://lxp.complote.com/api/device/terminal/activation-code";
    ESP_LOGI(TAG, "POST activation-code code=%s serial=%s", args->code.c_str(), mac_hex.c_str());
    auto opened = http->Open("POST", url);
    if (!opened) {
        ESP_LOGE(TAG, "activation-code HTTP open failed: %s", opened.error().ToString().c_str());
        cleanup();
        return;
    }

    auto status = http->GetStatusCode();
    std::string resp = http->ReadAll();
    http->Close();

    if (!status || *status < 200 || *status >= 300) {
        ESP_LOGW(TAG, "activation-code report fail HTTP %d: %s", status ? *status : -1, resp.c_str());
    } else {
        ESP_LOGI(TAG, "activation-code report OK: %s", resp.c_str());
    }
    cleanup();
}

}  // namespace

class LxpEsp32Board : public WifiBoard {
private:
    Button boot_button_;
    esp_timer_handle_t order_timer_ = nullptr;
    esp_timer_handle_t sense_timer_ = nullptr;
    int pir_last_level_ = -1;
    int radar_log_div_ = 0;
    bool use_radar_ = false;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            ESP_LOGI(TAG, "BOOT click, state=%d", (int)app.GetDeviceState());
            // Activating 期间短按不得进配网，否则会打断验证码播报
            if (app.GetDeviceState() == kDeviceStateActivating) {
                ESP_LOGI(TAG, "BOOT short-press ignored during activating (long-press / triple-click → WiFi config)");
                return;
            }
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "BOOT long-press → WiFi config");
            EnterWifiConfigMode();
        });
        // 三击 BOOT 也可进配网（激活卡住时的兜底）
        boot_button_.OnMultipleClick(
            [this]() {
                ESP_LOGI(TAG, "BOOT triple-click → WiFi config");
                EnterWifiConfigMode();
            },
            3);
    }

    void ScheduleOrderBridge() {
        esp_timer_create_args_t args = {
            .callback =
                [](void* /*arg*/) {
                    LxpOrderBridge::GetInstance().Start();
                },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "lxp_order",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &order_timer_) == ESP_OK) {
            esp_timer_start_once(order_timer_, 10 * 1000 * 1000ULL);
        }
    }

    void PollPir() {
        int level = gpio_get_level(LXP_PIR_GPIO);
        // HC-SR501：高电平表示检测到人（上升沿触发）
        if (pir_last_level_ >= 0 && level == 1 && pir_last_level_ == 0) {
            LxpOrderBridge::GetInstance().SpeakWelcome();
        }
        pir_last_level_ = level;
    }

    void PollRadar() {
        auto r = LxpLd2450::GetInstance().Poll();
        if (++radar_log_div_ >= 25) {  // ~5s at 200ms
            radar_log_div_ = 0;
            uint32_t rx = 0, ok = 0, bad = 0;
            LxpLd2450::GetInstance().GetDiag(&rx, &ok, &bad);
            if (r.present) {
                ESP_LOGI(TAG, "LD2450 present dist=%dmm speed=%d x=%d y=%d | rx=%u frames=%u bad=%u",
                         r.nearest_mm, r.speed_mm_s, r.x_mm, r.y_mm, (unsigned)rx, (unsigned)ok,
                         (unsigned)bad);
            } else if (rx == 0) {
                ESP_LOGW(TAG,
                         "LD2450 no UART data (rx=0). Check: module TX->RX2(GPIO16), VCC=5V, GND, "
                         "baud 256000");
            } else {
                ESP_LOGI(TAG, "LD2450 no target | rx=%u frames=%u bad=%u (wave hand in front)",
                         (unsigned)rx, (unsigned)ok, (unsigned)bad);
            }
        }
        if (r.present && r.nearest_mm > 0 && r.nearest_mm < LXP_LD2450_WELCOME_MAX_MM) {
            LxpOrderBridge::GetInstance().SpeakWelcome();
        }
    }

    void OnSenseTimer() {
        if (use_radar_) {
            PollRadar();
        } else {
            PollPir();
        }
    }

    void InitializePir() {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << LXP_PIR_GPIO;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_DISABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PIR gpio_config failed: %s", esp_err_to_name(err));
            return;
        }
        pir_last_level_ = gpio_get_level(LXP_PIR_GPIO);
        use_radar_ = false;
        ESP_LOGI(TAG, "PIR on GPIO%d", (int)LXP_PIR_GPIO);
    }

    void InitializePresenceSensor() {
#if LXP_LD2450_ENABLE
        if (LxpLd2450::GetInstance().Start()) {
            use_radar_ = true;
        } else {
            ESP_LOGW(TAG, "LD2450 start failed, falling back to PIR");
            InitializePir();
        }
#else
        InitializePir();
#endif
        esp_timer_create_args_t args = {
            .callback =
                [](void* arg) {
                    static_cast<LxpEsp32Board*>(arg)->OnSenseTimer();
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "lxp_sense",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &sense_timer_) == ESP_OK) {
            esp_timer_start_periodic(sense_timer_, 200 * 1000ULL);  // 200ms
            if (use_radar_) {
                ESP_LOGI(TAG, "Presence timer: LD2450 every 200ms (max %dmm)",
                         LXP_LD2450_WELCOME_MAX_MM);
            } else {
                ESP_LOGI(TAG, "Presence timer: PIR every 200ms");
            }
        }
    }

protected:
    std::string GetSsidPrefix() const override { return "dianxiao2"; }

    bool PrefetchActivationOnWifiConfig() const override { return true; }

    WifiLxpClaimResult OnLxpShopClaim(const std::string& pairing_code) override {
        return LxpClaimShop(pairing_code);
    }

    void OnEnterWifiConfigMode() override {
        // 重新配网时清本地绑定，并换新 Client-Id，便于 OTA 重新下发 6 位验证码
        ESP_LOGW(TAG, "Clearing mqtt/websocket binding + resetting UUID for re-provision");
        {
            Settings mqtt("mqtt", true);
            mqtt.EraseAll();
        }
        {
            Settings websocket("websocket", true);
            websocket.EraseAll();
        }
        ResetUuid();
        ESP_LOGI(TAG, "Wake word display name: %s", kWakeWordDisplay);
    }

    void InitializeTools() {
#if LXP_LD2450_ENABLE
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool(
            "self.radar.get_status",
            "Query HLK-LD2450 millimetre-wave radar: presence, distance(mm), speed(mm/s), "
            "x/y position(mm). Use when user asks about radar, distance, speed, or whether "
            "someone is in front of the device.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto r = LxpLd2450::GetInstance().Poll();
                uint32_t rx = 0, ok = 0, bad = 0;
                LxpLd2450::GetInstance().GetDiag(&rx, &ok, &bad);
                char buf[256];
                if (rx == 0) {
                    snprintf(buf, sizeof(buf),
                             "{\"present\":false,\"rx_bytes\":0,\"hint\":\"no UART data — wire "
                             "module TX to board RX2(GPIO16), VCC 5V, GND\"}");
                } else if (!r.present) {
                    snprintf(buf, sizeof(buf),
                             "{\"present\":false,\"rx_bytes\":%u,\"frames_ok\":%u,\"frames_bad\":%u,"
                             "\"message\":\"radar linked but no target\"}",
                             (unsigned)rx, (unsigned)ok, (unsigned)bad);
                } else {
                    snprintf(buf, sizeof(buf),
                             "{\"present\":true,\"distance_mm\":%d,\"speed_mm_s\":%d,"
                             "\"x_mm\":%d,\"y_mm\":%d,\"rx_bytes\":%u,"
                             "\"message\":\"target at %dmm, speed %dmm/s\"}",
                             r.nearest_mm, r.speed_mm_s, r.x_mm, r.y_mm, (unsigned)rx,
                             r.nearest_mm, r.speed_mm_s);
                }
                return std::string(buf);
            });
#endif
    }

public:
    // active_low BOOT, 长按 3 秒进配网
    LxpEsp32Board() : WifiBoard(), boot_button_(BOOT_BUTTON_GPIO, false, 3000, 0) {
        InitializeButtons();
        InitializePresenceSensor();
        InitializeTools();
        ScheduleOrderBridge();
    }

    ~LxpEsp32Board() {
        if (sense_timer_) {
            esp_timer_stop(sense_timer_);
            esp_timer_delete(sense_timer_);
            sense_timer_ = nullptr;
        }
        if (order_timer_) {
            esp_timer_stop(order_timer_);
            esp_timer_delete(order_timer_);
            order_timer_ = nullptr;
        }
    }

    void OnActivationCode(const std::string& code, const std::string& message) override {
        if (code.empty()) {
            return;
        }
        auto* args = new ActivationReportArgs{code, message};
        BaseType_t ok = xTaskCreate(ActivationReportTask, "lxp_act_code", 6144, args, 5, nullptr);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "activation-code report task create failed");
            delete args;
        }
    }

    virtual Led* GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, BUILTIN_LED_NUM);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        static NoDisplay display;
        return &display;
    }
};

DECLARE_BOARD(LxpEsp32Board);
