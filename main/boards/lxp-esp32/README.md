# 邻小铺 LXP 报单终端（基于小智 ESP-IDF）

硬件：ESP32-D0WD + MAX98357 + INMP441 + WS2812×24 + **HLK-LD2450**（可选 HC-SR501 回退；脚位见 `config.h`）

## 与小智的关系

本板复用小智完整音频栈（Opus / WebSocket / AudioService），语音大脑走 **xiao2.complote.com**。  
店铺订单 / 店内播报走 LXP MQTT，用 `[device_call]` 做通顺 Opus 播报。

详见 LXP 仓库：`docs/XIAOZHI-VS-LXP-ARCHITECTURE.md`

## 硬件接线（HLK-LD2450 毫米波）

默认 `LXP_LD2450_ENABLE=1`，用海凌科 LD2450 做进店检测：

| 模块 | 信号 | ESP32 | 说明 |
|------|------|-------|------|
| **HLK-LD2450** | TX | **RX2 / GPIO16**（`LXP_LD2450_RX_GPIO`） | 模块 → 板子；旁路 D4 是另一脚，勿接；不占用 USB |
| | RX | 可不接 | 固件不向雷达发命令 |
| | VCC | **5V** | |
| | GND | GND | 共地 |

- UART：`UART_NUM_1` 重映射到 **GPIO16（RX2）**，波特率 **256000**（`LXP_LD2450_BAUD`）
- 欢迎距离阈值：`LXP_LD2450_WELCOME_MAX_MM`（默认 **2500 mm**），目标在雷达前方（y>0）且距离小于阈值时触发欢迎
- **USB**：走 RX0/TX0，与 RX2 无关，烧录与监视器正常
- **测试**：串口每约 5 秒打 `LD2450 present dist=…mm speed=… x=… y=…`；也可语音问「雷达有没有人 / 距离多少」，走 MCP `self.radar.get_status`

### PIR 回退（HC-SR501）

将 `config.h` 中 `LXP_LD2450_ENABLE` 设为 `0` 后编译，走 PIR：

| 模块 | 信号 | ESP32 | 说明 |
|------|------|-------|------|
| **HC-SR501** | OUT | **GPIO34**（`LXP_PIR_GPIO`） | 仅输入；上升沿触发进店欢迎 |
| | VCC | **5V** | |
| | GND | GND | 共地 |

麦 / 功放 / 灯环脚位与 Arduino `lxp-order-terminal` 对齐（见 `config.h`）。

进店欢迎依赖 NVS `lxp`：`welcome_text` / `welcome_en` / `pir_en` / `welcome_cd`（秒，默认 60）。  
由 MQTT `linshop/device/{id}/config` 的 `DEVICE_CONFIG` 下发并写入。

## MQTT

连接后订阅：

- `linshop/shop/{shop}/orders/#` — 订单报单
- `linshop/shop/{shop}/announce` — 播报 / 欢迎 / 核销 / 充值 / BGM 提示
- `linshop/device/{id}/config` — 欢迎词与 PIR 开关等

`announce`：`ANNOUNCE` / `WELCOME` / `ORDER_VERIFIED` / `WALLET_RECHARGE` → TTS；  
`PLAY_BGM` → 无 HTTP mp3 播放器时播报 `speakText` 或「正在播放背景音乐」；  
`STOP_SPEAK` / `STOP_BGM` → 打断当前播报。

## 编译

```bash
source ~/esp/esp-idf/export.sh
cd frido/firmware/xiaozhi-esp32
python scripts/build.py lxp-esp32
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

## 配置 / 激活 / 绑店

1. 首次上电或长按 BOOT 配网：手机连 SoftAP **`dianxiao2-XXXX`** → 浏览器打开 `http://192.168.4.1`
2. 在「智能配网」页填写 **WiFi SSID + 密码**，并（可选）填写 **本店配对码**（商户后台「店小二终端」生成，可粘贴 `LXPTERM:XXXX`）
3. 点连接：WiFi 成功后，若填了配对码，设备会在 STA 仍在线时 `POST /api/device/terminal/claim` 绑店，完成页显示「已绑定店铺：xxx」
4. 同一次配网还会尽量展示 **xiao2 6 位验证码**（设备同时语音播报两遍）；热点约再保持 ~25 秒后关闭
5. 在 https://xiao2.complote.com/ 输入验证码激活语音通道（与绑店独立）
6. 绑店成功后 NVS `lxp` 已写入：
   - `mqtt_host` 默认 `mqtt.complote.com`
   - `mqtt_port` 默认 `1883`
   - `mqtt_user` / `mqtt_pass` / `shop_id` / `device_id`

> **长按 BOOT 只是配网**；填配对码才是绑店。只配 WiFi 不填码时，订单 MQTT 仍未绑定。

### 重新配网与新验证码

长按 / 三击 BOOT 进入配网时，固件会：

- 清除本地 `mqtt` / `websocket` 绑定配置
- 重新生成 `Client-Id`（uuid）

以便下次 OTA 检查时重新下发激活码。若控制台仍显示已绑定，请在 xiao2 侧解绑后再激活。重新绑店请在配网页再次填写新的本店配对码。

## 按键

- BOOT **短按**：开/关对话；**激活中（Activating）短按忽略**（避免打断验证码播报）
- BOOT **长按 3 秒**：重新配网（开热点）
- BOOT **连按 3 次**：重新配网（兜底）

启动阶段（Starting、尚未激活）短按仍可进配网。

## SoftAP

SSID 前缀：`dianxiao2`（例如 `dianxiao2-A1B2`）。

## 唤醒词「你好小二」

产品唤醒/展示名：**你好小二**（兼容口语「你好店小二」）。

**经典 ESP32-D0WD（本板默认）:** `CONFIG_WAKE_WORD_DISABLED=y`，对话靠 BOOT 短按；播报中 BOOT / MQTT `STOP_SPEAK`/`STOP_BGM` 可打断（见 `Application::AbortSpeaking`）。

**ESP32-S3 + PSRAM（丝滑主推）:** 另建 `lxp-esp32-s3` 构建，打开：
- `CONFIG_USE_ESP_WAKE_WORD` 或 `CONFIG_USE_CUSTOM_WAKE_WORD`
- 自定义词「你好小二」Multinet/WakeNet 模型
- speaking 态 AFE 检测以支持 **播报/BGM 中 barge-in** → `AbortSpeaking(kAbortReasonWakeWordDetected)`

详见 LXP：`firmware/lxp-xiaozhi-terminal/README.md`、`docs/XIAO2-LATENCY.md`。

若当前无可用的端侧「你好小二」WN 模型，先完成 xiao2 WS 通顺对话，再补定制模型。

## 配网完成页补丁说明

为了在手机上展示验证码与邻小铺绑店，对依赖 `78/esp-wifi-connect` 做了本地增强（`managed_components/78__esp-wifi-connect`）：

- WiFi 试连成功且拿到 IP 后预拉 OTA 激活码
- 配网页增加「邻小铺绑店」配对码字段；提交后 STA 在线时 claim，响应带 `shop_bound` / `shop_name`
- `/activation.json` + `wifi_configuration_done.html`：大号等宽验证码、每 500ms 轮询至约 25s、一键复制；绑店文案来自 URL `message`

若执行 `idf.py update-dependencies` 覆盖该组件，需重新应用上述改动。
