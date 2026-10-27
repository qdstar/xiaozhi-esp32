#pragma once

#include <memory>
#include <string>

/** LXP 店铺订单 / 播报 MQTT → 小智 [device_call] Opus TTS */
class LxpOrderBridge {
public:
    static LxpOrderBridge& GetInstance();
    void Start();
    void Stop();

    /** PIR 进店欢迎：读 NVS lxp 欢迎词设置，带冷却 */
    void SpeakWelcome();

private:
    LxpOrderBridge() = default;
    void Connect();
    void HandlePayload(const std::string& payload);
    void HandleDeviceConfig(const std::string& payload);
    void SpeakOrder(const std::string& text);

    std::unique_ptr<class Mqtt> mqtt_;
    std::string last_order_key_;
    int64_t last_order_ms_ = 0;
    int64_t last_welcome_ms_ = 0;
    bool started_ = false;
};
