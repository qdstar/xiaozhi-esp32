#include "lxp_shop_claim.h"

#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cctype>
#include <cstdlib>
#include <string>

#define TAG "LxpShopClaim"

namespace {

std::string TrimCopy(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string ToUpperAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string MacHexUpper() {
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

/** Strip LXPTERM: / URL ?c= and uppercase. */
std::string NormalizePairingCode(const std::string& raw_in) {
    std::string raw = TrimCopy(raw_in);
    if (raw.empty()) {
        return {};
    }

    // LXPTERM:XXXX
    if (raw.size() > 8) {
        std::string prefix = raw.substr(0, 8);
        for (char& c : prefix) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (prefix == "LXPTERM:") {
            raw = TrimCopy(raw.substr(8));
        }
    }

    // https://...?...&c=XXXX or ?c=XXXX
    auto c_pos = raw.find("c=");
    if (c_pos != std::string::npos && (c_pos == 0 || raw[c_pos - 1] == '?' || raw[c_pos - 1] == '&')) {
        std::string rest = raw.substr(c_pos + 2);
        auto amp = rest.find('&');
        if (amp != std::string::npos) {
            rest = rest.substr(0, amp);
        }
        raw = TrimCopy(rest);
    }

    raw = ToUpperAscii(raw);
    if (raw.size() < 6 || raw.size() > 16) {
        return {};
    }
    for (char c : raw) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            return {};
        }
    }
    return raw;
}

void ParseMqttEndpoint(const std::string& mqtt_url_or_host, std::string& host_out, int& port_out) {
    host_out = "mqtt.complote.com";
    port_out = 1883;

    std::string s = TrimCopy(mqtt_url_or_host);
    if (s.empty()) {
        return;
    }

    // mqtt://host:port[/...]
    if (s.rfind("mqtt://", 0) == 0) {
        s = s.substr(7);
    } else if (s.rfind("mqtts://", 0) == 0) {
        s = s.substr(8);
        port_out = 8883;
    }

    auto slash = s.find('/');
    if (slash != std::string::npos) {
        s = s.substr(0, slash);
    }

    auto colon = s.rfind(':');
    if (colon != std::string::npos && colon + 1 < s.size()) {
        host_out = s.substr(0, colon);
        int parsed = atoi(s.c_str() + colon + 1);
        if (parsed > 0 && parsed < 65536) {
            port_out = parsed;
        }
    } else if (!s.empty()) {
        host_out = s;
    }
}

std::string JsonErrorMessage(cJSON* root) {
    if (!root) {
        return "绑店响应无效";
    }
    cJSON* msg = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsString(msg) && msg->valuestring) {
        return msg->valuestring;
    }
    cJSON* err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(err) && err->valuestring) {
        return err->valuestring;
    }
    return "绑店失败";
}

}  // namespace

WifiLxpClaimResult LxpClaimShop(const std::string& pairing_code_raw) {
    WifiLxpClaimResult result;

    std::string code = NormalizePairingCode(pairing_code_raw);
    if (code.empty()) {
        result.message = "配对码无效";
        return result;
    }

    auto* network = Board::GetInstance().GetNetwork();
    if (!network) {
        result.message = "网络不可用";
        return result;
    }

    auto http = network->CreateHttp(0);
    if (!http) {
        result.message = "无法创建 HTTP 客户端";
        return result;
    }

    std::string mac_hex = MacHexUpper();
    std::string mac_colons = SystemInfo::GetMacAddress();

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "pairingCode", code.c_str());
    cJSON_AddStringToObject(body, "serialNo", mac_hex.c_str());
    cJSON_AddStringToObject(body, "firmware", "lxp-esp32");
    cJSON_AddStringToObject(body, "mac", mac_colons.c_str());
    cJSON_AddStringToObject(body, "xiaozhiDeviceId", mac_hex.c_str());
    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) {
        result.message = "构造请求失败";
        return result;
    }

    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetTimeout(20000);
    http->SetContent(std::string(body_str));
    cJSON_free(body_str);

    const char* url = "https://lxp.complote.com/api/device/terminal/claim";
    ESP_LOGI(TAG, "POST claim pairing_len=%u serial=%s", (unsigned)code.size(), mac_hex.c_str());
    auto opened = http->Open("POST", url);
    if (!opened) {
        ESP_LOGE(TAG, "HTTP open failed: %s", opened.error().ToString().c_str());
        result.message = "绑店请求失败（网络）";
        return result;
    }

    auto status = http->GetStatusCode();
    std::string resp = http->ReadAll();
    http->Close();

    if (!status) {
        result.message = "绑店无响应";
        return result;
    }
    if (*status < 200 || *status >= 300) {
        ESP_LOGW(TAG, "claim HTTP %d body_len=%u", *status, (unsigned)resp.size());
        cJSON* err_root = cJSON_Parse(resp.c_str());
        result.message = JsonErrorMessage(err_root);
        cJSON_Delete(err_root);
        return result;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (!root) {
        result.message = "绑店响应解析失败";
        return result;
    }

    cJSON* payload = root;
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsObject(data)) {
        payload = data;
    }

    cJSON* username = cJSON_GetObjectItem(payload, "username");
    cJSON* password = cJSON_GetObjectItem(payload, "password");
    cJSON* shop_id = cJSON_GetObjectItem(payload, "shopId");
    cJSON* device_id = cJSON_GetObjectItem(payload, "deviceId");
    if (!cJSON_IsString(device_id) || !device_id->valuestring || !device_id->valuestring[0]) {
        device_id = cJSON_GetObjectItem(payload, "id");
    }
    cJSON* shop_name = cJSON_GetObjectItem(payload, "shopName");
    cJSON* mqtt_url = cJSON_GetObjectItem(payload, "mqttUrl");
    cJSON* host_item = cJSON_GetObjectItem(payload, "host");

    if (!cJSON_IsString(username) || !cJSON_IsString(password) || !cJSON_IsString(shop_id) ||
        !cJSON_IsString(device_id) || !username->valuestring || !password->valuestring ||
        !shop_id->valuestring || !device_id->valuestring) {
        result.message = JsonErrorMessage(payload);
        cJSON_Delete(root);
        return result;
    }

    std::string mqtt_host = "mqtt.complote.com";
    int mqtt_port = 1883;
    if (cJSON_IsString(mqtt_url) && mqtt_url->valuestring && mqtt_url->valuestring[0]) {
        ParseMqttEndpoint(mqtt_url->valuestring, mqtt_host, mqtt_port);
    } else if (cJSON_IsString(host_item) && host_item->valuestring && host_item->valuestring[0]) {
        ParseMqttEndpoint(host_item->valuestring, mqtt_host, mqtt_port);
    }

    {
        Settings s("lxp", true);
        s.SetString("mqtt_host", mqtt_host);
        s.SetInt("mqtt_port", mqtt_port);
        s.SetString("mqtt_user", username->valuestring);
        s.SetString("mqtt_pass", password->valuestring);
        s.SetString("shop_id", shop_id->valuestring);
        s.SetString("device_id", device_id->valuestring);
        if (cJSON_IsString(shop_name) && shop_name->valuestring) {
            s.SetString("shop_name", shop_name->valuestring);
        }
    }

    result.ok = true;
    result.shop_name = (cJSON_IsString(shop_name) && shop_name->valuestring) ? shop_name->valuestring : "";
    if (!result.shop_name.empty()) {
        result.message = std::string("已绑定店铺：") + result.shop_name;
    } else {
        result.message = "已绑定店铺";
    }
    ESP_LOGI(TAG, "Claim OK shop=%s device=%s mqtt=%s:%d", result.shop_name.c_str(),
             device_id->valuestring, mqtt_host.c_str(), mqtt_port);

    cJSON_Delete(root);
    return result;
}

void LxpTryClaimPendingPairing() {
    Settings s("lxp", true);
    auto pending = s.GetString("pending_pairing");
    if (pending.empty()) {
        return;
    }
    ESP_LOGI(TAG, "Retrying pending_pairing claim…");
    auto r = LxpClaimShop(pending);
    if (r.ok) {
        s.EraseKey("pending_pairing");
        ESP_LOGI(TAG, "pending_pairing claim OK, cleared NVS");
    } else {
        ESP_LOGW(TAG, "pending_pairing claim failed: %s", r.message.c_str());
    }
}
