#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <wifi_configuration_ap.h>

class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    bool in_config_mode_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;

    virtual std::string GetBoardJson() override;

    /**
     * SoftAP SSID prefix (hostname uses the same prefix). Override per board.
     */
    virtual std::string GetSsidPrefix() const { return "Xiaozhi"; }

    /**
     * When true, after captive-portal WiFi test succeeds, prefetch OTA activation
     * code so the done page can show the 6-digit code on the phone.
     */
    virtual bool PrefetchActivationOnWifiConfig() const { return false; }

    /**
     * When true, SoftAP Wi-Fi submit may also POST LXP shop claim with pairing_code.
     * Defaults to same gate as activation prefetch (lxp-esp32).
     */
    virtual bool EnableLxpShopClaimOnWifiConfig() const { return PrefetchActivationOnWifiConfig(); }

    /**
     * Hook before entering WiFi config (e.g. clear binding to force new activation).
     */
    virtual void OnEnterWifiConfigMode() {}

    /**
     * SoftAP optional LXP shop claim. Override on boards that implement claim
     * (see lxp-esp32/lxp_shop_claim).
     */
    virtual WifiLxpClaimResult OnLxpShopClaim(const std::string& pairing_code);

    /**
     * Handle network event (called from WiFi manager callbacks)
     * @param event The network event type
     * @param data Additional data (e.g., SSID for Connecting/Connected events)
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     * Start WiFi connection attempt
     */
    void TryWifiConnect();

    /**
     * Enter WiFi configuration mode
     */
    void StartWifiConfigMode();

    /**
     * WiFi connection timeout callback
     */
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * Start network connection asynchronously
     * This function returns immediately. Network events are notified through the callback set by SetNetworkEventCallback().
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    
    /**
     * Enter WiFi configuration mode (thread-safe, can be called from any task)
     */
    void EnterWifiConfigMode();
    
    /**
     * Check if in WiFi config mode
     */
    bool IsInWifiConfigMode() const;
};

#endif // WIFI_BOARD_H
