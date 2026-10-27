#include "lxp_order_bridge.h"

#include "application.h"
#include "board.h"
#include "device_state.h"
#include "lxp_shop_claim.h"
#include "mqtt.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <string>

#define TAG "LxpOrder"

LxpOrderBridge& LxpOrderBridge::GetInstance() {
    static LxpOrderBridge instance;
    return instance;
}

void LxpOrderBridge::SpeakOrder(const std::string& text) {
    if (text.empty()) {
        return;
    }
    // 服务端 listen/detect + [device_call] → 直接 TTS Opus 推流（不经 LLM）
    std::string payload = std::string("[device_call]") + text;
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        app.AbortSpeaking(kAbortReasonNone);
        app.Schedule([payload]() {
            vTaskDelay(pdMS_TO_TICKS(200));
            Application::GetInstance().WakeWordInvoke(payload);
        });
    } else {
        app.WakeWordInvoke(payload);
    }
}

void LxpOrderBridge::SpeakWelcome() {
    Settings s("lxp", false);
    if (!s.GetBool("pir_en", false) || !s.GetBool("welcome_en", false)) {
        return;
    }
    auto text = s.GetString("welcome_text");
    if (text.empty()) {
        return;
    }
    int cd = s.GetInt("welcome_cd", 60);
    if (cd < 10) {
        cd = 10;
    }
    if (cd > 3600) {
        cd = 3600;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_welcome_ms_ < (int64_t)cd * 1000) {
        return;
    }
    last_welcome_ms_ = now;
    ESP_LOGI(TAG, "PIR welcome: %s", text.c_str());
    SpeakOrder(text);
}

void LxpOrderBridge::HandleDeviceConfig(const std::string& payload) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        return;
    }
    const char* type = "";
    auto* t = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(t)) {
        type = t->valuestring;
    }
    if (strcmp(type, "DEVICE_CONFIG") != 0) {
        cJSON_Delete(root);
        return;
    }

    Settings s("lxp", true);
    bool changed = false;

    auto* we = cJSON_GetObjectItem(root, "welcomeEnabled");
    if (cJSON_IsBool(we)) {
        s.SetBool("welcome_en", cJSON_IsTrue(we));
        changed = true;
    }
    auto* wt = cJSON_GetObjectItem(root, "welcomeText");
    if (cJSON_IsString(wt) && wt->valuestring) {
        std::string text = wt->valuestring;
        if (text.size() > 200) {
            text.resize(200);
        }
        s.SetString("welcome_text", text);
        changed = true;
    }
    auto* pe = cJSON_GetObjectItem(root, "pirEnabled");
    if (cJSON_IsBool(pe)) {
        s.SetBool("pir_en", cJSON_IsTrue(pe));
        changed = true;
    }
    auto* cd = cJSON_GetObjectItem(root, "welcomeCooldownSec");
    if (cJSON_IsNumber(cd)) {
        int v = cd->valueint;
        if (v < 10) {
            v = 10;
        }
        if (v > 3600) {
            v = 3600;
        }
        s.SetInt("welcome_cd", v);
        changed = true;
    }
    // 音量 / 灯光由小智自有设置管；此处只记欢迎相关
    if (changed) {
        auto text = s.GetString("welcome_text");
        ESP_LOGI(TAG, "DEVICE_CONFIG welcome_en=%d pir_en=%d cd=%d text=%s",
                 s.GetBool("welcome_en", false) ? 1 : 0, s.GetBool("pir_en", false) ? 1 : 0,
                 (int)s.GetInt("welcome_cd", 60), text.c_str());
    }
    cJSON_Delete(root);
}

void LxpOrderBridge::HandlePayload(const std::string& payload) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        return;
    }
    const char* type = "";
    auto* t = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(t)) {
        type = t->valuestring;
    }

    if (strcmp(type, "STOP_SPEAK") == 0 || strcmp(type, "STOP_BGM") == 0) {
        ESP_LOGI(TAG, "%s -> AbortSpeaking", type);
        Application::GetInstance().AbortSpeaking(kAbortReasonNone);
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "ANNOUNCE") == 0 || strcmp(type, "WELCOME") == 0 ||
        strcmp(type, "ORDER_VERIFIED") == 0 || strcmp(type, "WALLET_RECHARGE") == 0) {
        const char* speak = "";
        auto* st = cJSON_GetObjectItem(root, "speakText");
        if (cJSON_IsString(st)) {
            speak = st->valuestring;
        }
        if (speak[0]) {
            ESP_LOGI(TAG, "announce %s speak=%s", type, speak);
            SpeakOrder(speak);
        }
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "PLAY_BGM") == 0) {
        const char* url = "";
        auto* u = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(u)) {
            url = u->valuestring;
        }
        const char* speak = "";
        auto* st = cJSON_GetObjectItem(root, "speakText");
        if (cJSON_IsString(st)) {
            speak = st->valuestring;
        }
        ESP_LOGI(TAG, "PLAY_BGM url=%s (no HTTP mp3 player; TTS tip)", url);
        std::string tip = speak[0] ? speak : "正在播放背景音乐";
        SpeakOrder(tip);
        cJSON_Delete(root);
        return;
    }

    bool paid = strcmp(type, "ORDER_PAID") == 0;
    bool neu = strcmp(type, "ORDER_NEW") == 0;
    if (!paid && !neu) {
        cJSON_Delete(root);
        return;
    }

    const char* speak = "";
    auto* st = cJSON_GetObjectItem(root, "speakText");
    if (cJSON_IsString(st)) {
        speak = st->valuestring;
    }

    std::string order_id;
    std::string pickup;
    auto* order = cJSON_GetObjectItem(root, "order");
    if (cJSON_IsObject(order)) {
        auto* oid = cJSON_GetObjectItem(order, "orderId");
        if (!cJSON_IsString(oid)) {
            oid = cJSON_GetObjectItem(order, "id");
        }
        if (cJSON_IsString(oid)) {
            order_id = oid->valuestring;
        }
        auto* pc = cJSON_GetObjectItem(order, "pickupCode");
        if (cJSON_IsString(pc)) {
            pickup = pc->valuestring;
        }
    }

    std::string key = std::string(type) + ":" + (order_id.empty() ? pickup : order_id);
    int64_t now = esp_timer_get_time() / 1000;
    if (key == last_order_key_ && now - last_order_ms_ < 8000) {
        ESP_LOGI(TAG, "skip dup order %s", key.c_str());
        cJSON_Delete(root);
        return;
    }
    last_order_key_ = key;
    last_order_ms_ = now;

    std::string say = speak;
    if (say.empty()) {
        say = (paid ? "新订单已支付，取货码 " : "新订单，取货码 ") + pickup;
    }
    ESP_LOGI(TAG, "order %s speak=%s", type, say.c_str());
    SpeakOrder(say);
    cJSON_Delete(root);
}

void LxpOrderBridge::Connect() {
    // SoftAP /submit may have deferred claim; retry pending_pairing then re-read creds.
    LxpTryClaimPendingPairing();

    Settings s("lxp", false);
    auto host = s.GetString("mqtt_host", "mqtt.complote.com");
    int port = s.GetInt("mqtt_port", 1883);
    auto user = s.GetString("mqtt_user");
    auto pass = s.GetString("mqtt_pass");
    auto shop = s.GetString("shop_id");
    auto device = s.GetString("device_id");
    if (user.empty() || pass.empty() || shop.empty()) {
        ESP_LOGW(TAG, "LXP MQTT 未配置（NVS ns=lxp: mqtt_user/mqtt_pass/shop_id），仅语音对话可用");
        return;
    }

    auto network = Board::GetInstance().GetNetwork();
    if (!network) {
        return;
    }
    mqtt_ = network->CreateMqtt(1);
    if (!mqtt_) {
        ESP_LOGE(TAG, "CreateMqtt failed");
        return;
    }
    mqtt_->SetKeepAlive(60);
    mqtt_->OnConnected([this, shop, device]() {
        ESP_LOGI(TAG, "LXP MQTT connected, subscribe shop=%s", shop.c_str());
        std::string topic = "linshop/shop/" + shop + "/orders/#";
        mqtt_->Subscribe(topic, 1);
        std::string announce = "linshop/shop/" + shop + "/announce";
        mqtt_->Subscribe(announce, 1);
        if (!device.empty()) {
            mqtt_->Subscribe("linshop/device/" + device + "/orders/#", 1);
            mqtt_->Subscribe("linshop/device/" + device + "/config", 1);
            mqtt_->Subscribe("linshop/device/" + device + "/cmd", 1);
        }
    });
    mqtt_->OnDisconnected([]() { ESP_LOGW(TAG, "LXP MQTT disconnected"); });
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        ESP_LOGI(TAG, "mqtt %s (%u bytes)", topic.c_str(), (unsigned)payload.size());
        if (topic.find("/config") != std::string::npos) {
            HandleDeviceConfig(payload);
            return;
        }
        if (topic.find("/announce") != std::string::npos || topic.find("/orders/") != std::string::npos) {
            HandlePayload(payload);
        }
    });

    std::string client_id = device.empty() ? ("lxp-xz-" + user) : device;
    ESP_LOGI(TAG, "Connecting LXP MQTT %s:%d as %s", host.c_str(), port, user.c_str());
    auto r = mqtt_->Connect(host, port, client_id, user, pass);
    if (!r) {
        ESP_LOGE(TAG, "LXP MQTT connect failed");
        mqtt_.reset();
    }
}

void LxpOrderBridge::Start() {
    if (started_) {
        return;
    }
    started_ = true;
    Application::GetInstance().Schedule([this]() { Connect(); });
}

void LxpOrderBridge::Stop() {
    if (mqtt_) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }
    started_ = false;
}
