#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "board.h"
#include "button.h"
#include "config.h"
#include "device_state.h"

#include <esp_log.h>
#include "i2c_device.h"
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <wifi_station.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp32_camera.h"

#include "esp_lcd_panel_vendor.h"
#include "custom_io_expander_ch32v003.h"
#include "esp_lcd_st7796.h"
#include "esp_ota_ops.h"
#include "lxp_order_bridge.h"
#include "lxp_shop_claim.h"
#include "settings.h"

#include <esp_heap_caps.h>

#include <cmath>
#include <vector>

#define TAG "waveshare_s3_cam_xxxx"

#define LCD_OPCODE_WRITE_CMD        (0x02ULL)
#define LCD_OPCODE_READ_CMD         (0x0BULL)
#define LCD_OPCODE_WRITE_COLOR      (0x32ULL)

void switch_to_main(void)
{
    const esp_partition_t *factory_part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                 NULL);
    if (!factory_part) {
        ESP_LOGE("APP_SWITCH", "未找到 factory 分区");
        return;
    }

    esp_err_t err = esp_ota_set_boot_partition(factory_part);
    if (err == ESP_OK) {
        ESP_LOGI("APP_SWITCH", "已设置 factory 为启动分区，重启回到主程序");
        esp_restart();
    } else {
        ESP_LOGE("APP_SWITCH", "设置启动分区失败: %s", esp_err_to_name(err));
    }
}

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_io_expander_handle_t io_handle)
        : Backlight(), io_handle_(io_handle) {}

    void ForceOn() {
        // CH32 的 LCD_BL 低电平点亮，0 最亮，255 全灭。
        for (int i = 0; i < 8; ++i) {
            if (custom_io_expander_set_pwm(io_handle_, 0) == ESP_OK) {
                ESP_LOGI(TAG, "LCD backlight PWM 0 (full on) ok");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        ESP_LOGE(TAG, "LCD backlight PWM failed");
    }

protected:
    esp_io_expander_handle_t io_handle_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        if (brightness > 100) brightness = 100;
        uint8_t pwm = (100 - brightness) * 255 / 100;
        for (int i = 0; i < 4; ++i) {
            if (custom_io_expander_set_pwm(io_handle_, pwm) == ESP_OK) {
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
};


class CustomBoard : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_;
    LcdDisplay* display_;
    Esp32Camera* camera_;
    esp_io_expander_handle_t io_expander = NULL;
    CustomBacklight *backlight_;
    esp_timer_handle_t order_timer_ = nullptr;

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

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = I2C_SDA_IO,
            .scl_io_num = I2C_SCL_IO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }
    
    void Initialize_Expander(void)
    {

        custom_io_expander_new_i2c_ch32v003(i2c_bus_, BSP_IO_EXPANDER_I2C_ADDRESS, &io_expander);
        esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_4 |  IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT);

        esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 , 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 , 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 , 1);
        vTaskDelay(pdMS_TO_TICKS(10));

        esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_4 | IO_EXPANDER_PIN_NUM_6 , 1);

        // EXIO5 = BAT_EN，高电平锁存电池。合上电池开关时 VSYS 会抖一下，CH32 可能复位并把
        // PA_CTRL / PWR_LED 拉低。先保持上面的正常输出，再把 BAT_EN 置 1，并在 1 秒内反复重写。
        esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_5, IO_EXPANDER_OUTPUT);
        esp_err_t bat_ret = esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_5, 1);
        if (bat_ret != ESP_OK) {
            ESP_LOGE(TAG, "BAT_EN set failed: %s", esp_err_to_name(bat_ret));
        }
        // 背光 PWM 和这块 I2C 共用 CH32。电池重写任务放到屏幕点亮之后再开。
    }

    void StartBatteryHold() {
        xTaskCreate(BatteryHoldTask, "bat_hold", 3072, this, 8, nullptr);
    }

    static void BacklightProbeTask(void* arg)
    {
        auto* io = static_cast<esp_io_expander_handle_t>(arg);
        const uint8_t steps[] = {100, 0, 255, 180, 40};
        for (int round = 0; round < 8; ++round) {
            for (uint8_t pwm : steps) {
                esp_err_t err = custom_io_expander_set_pwm(io, pwm);
                ESP_LOGW(TAG, "BL probe pwm=%u %s", pwm, err == ESP_OK ? "ok" : "fail");
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        custom_io_expander_set_pwm(io, 100);
        ESP_LOGW(TAG, "BL probe hold pwm=100");
        vTaskDelete(nullptr);
    }

    static void BatteryHoldTask(void* arg)
    {
        auto* self = static_cast<CustomBoard*>(arg);
        int fails = 0;
        for (int i = 0; i < 50; ++i) {
            if (custom_io_expander_rewrite(self->io_expander) != ESP_OK) {
                fails++;
            }
            custom_io_expander_set_pwm(self->io_expander, 100);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        uint16_t bat_adc = 0;
        if (custom_io_expander_get_adc(self->io_expander, &bat_adc) == ESP_OK) {
            float vbat = (bat_adc / 1023.0f) * 3.3f * 3.0f;
            ESP_LOGI(TAG, "BAT_EN held, ADC=%u (~%.2fV), rewrite fails=%d", bat_adc, vbat, fails);
        } else {
            ESP_LOGW(TAG, "BAT_EN held, ADC read failed, rewrite fails=%d", fails);
        }
        xTaskCreate(BacklightProbeTask, "bl_probe", 3072, self->io_expander, 5, nullptr);
        vTaskDelete(nullptr);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7789Display() {
        // LCD_RST = EXIO1，先拉低再释放，18pin 屏才能完成上电复位。
        esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_1, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_1, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        ESP_LOGI(TAG, "LCD panel T320H7-C18-02 ST7789 240x320");

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
        constexpr int kStripe = 20;
        auto* stripe = static_cast<uint16_t*>(
            heap_caps_malloc(DISPLAY_WIDTH * kStripe * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (stripe != nullptr) {
            for (int i = 0; i < DISPLAY_WIDTH * kStripe; ++i) {
                stripe[i] = 0xFFFF;
            }
            for (int y = 0; y < DISPLAY_HEIGHT; y += kStripe) {
                int y2 = y + kStripe;
                if (y2 > DISPLAY_HEIGHT) {
                    y2 = DISPLAY_HEIGHT;
                }
                esp_lcd_panel_draw_bitmap(panel, 0, y, DISPLAY_WIDTH, y2, stripe);
            }
            heap_caps_free(stripe);
            ESP_LOGI(TAG, "LCD white fill %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
        }
        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(io_expander);
        backlight_->ForceOn();
    }

        void InitializeSt7796Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel,true));
        display_ = new SpiLcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(io_expander);
        backlight_->RestoreBrightness();   
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (app.GetDeviceState() == kDeviceStateIdle) {
                ESP_LOGW(TAG, "BOOT → SpeakOrder 测试语音");
                LxpOrderBridge::GetInstance().SpeakOrder("你好，我是邻小铺报单终端");
            }
            app.ToggleChatState();
        });
        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "BOOT long-press → WiFi config");
            EnterWifiConfigMode();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    static void RunSpeakerSelfTest() {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (!codec) {
            ESP_LOGE(TAG, "SPEAKER_TEST: no codec");
            return;
        }
        codec->SetOutputVolume(70);
        codec->EnableOutput(true);
        ESP_LOGW(TAG, "SPEAKER_TEST: 1kHz 2s vol=70 (ES8311) — LISTEN");
        const int sr = AUDIO_OUTPUT_SAMPLE_RATE;
        const int chunk = sr / 10;
        std::vector<int16_t> buf(static_cast<size_t>(chunk));
        for (int n = 0; n < 20; ++n) {
            for (int i = 0; i < chunk; ++i) {
                float t = static_cast<float>(n * chunk + i) / static_cast<float>(sr);
                buf[static_cast<size_t>(i)] =
                    static_cast<int16_t>(18000.0f * sinf(2.0f * 3.1415926f * 1000.0f * t));
            }
            codec->OutputData(buf);
        }
        ESP_LOGW(TAG, "SPEAKER_TEST done");
    }

    void ScheduleAudioSelfTest() {
        xTaskCreate(
            [](void* /*arg*/) {
                vTaskDelay(pdMS_TO_TICKS(4000));
                // 未配网也会循环测音，方便先听喇叭
                for (int round = 1; round <= 20; ++round) {
                    ESP_LOGW(TAG, "SPEAKER_LOOP round=%d/20", round);
                    RunSpeakerSelfTest();
                    auto st = Application::GetInstance().GetDeviceState();
                    if (st == kDeviceStateIdle) {
                        ESP_LOGW(TAG, "SPEAK_TEST: 云端 TTS");
                        Application::GetInstance().WakeWordInvoke(
                            "[device_call]你好，我是邻小铺，S3摄像头板语音测试");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                vTaskDelete(nullptr);
            },
            "spk_test", 8192, nullptr, 5, nullptr);
    }


    void InitializeCamera() {
        camera_config_t camera_config = {
            .pin_pwdn = CAMERA_PIN_PWDN,
            .pin_reset = CAMERA_PIN_RESET,
            .pin_xclk = CAMERA_PIN_XCLK,
            .pin_sccb_sda = -1, // Use initialized I2C
            .pin_sccb_scl = -1,
            .pin_d7 = CAMERA_PIN_D7,
            .pin_d6 = CAMERA_PIN_D6,
            .pin_d5 = CAMERA_PIN_D5,
            .pin_d4 = CAMERA_PIN_D4,
            .pin_d3 = CAMERA_PIN_D3,
            .pin_d2 = CAMERA_PIN_D2,
            .pin_d1 = CAMERA_PIN_D1,
            .pin_d0 = CAMERA_PIN_D0,
            .pin_vsync = CAMERA_PIN_VSYNC,
            .pin_href = CAMERA_PIN_HREF,
            .pin_pclk = CAMERA_PIN_PCLK,

            .xclk_freq_hz = XCLK_FREQ_HZ,
            .ledc_timer = LEDC_TIMER_0,
            .ledc_channel = LEDC_CHANNEL_0,

            .pixel_format = PIXFORMAT_RGB565,
            .frame_size = FRAMESIZE_QVGA,
            .jpeg_quality = 12,
            .fb_count = 2,
            .fb_location = CAMERA_FB_IN_PSRAM,
            .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
            .sccb_i2c_port = (i2c_port_t)1,
        };

        camera_ = new Esp32Camera(camera_config);
    }
public:
protected:
    std::string GetSsidPrefix() const override { return "dianxiao2"; }

    bool PrefetchActivationOnWifiConfig() const override { return true; }

    WifiLxpClaimResult OnLxpShopClaim(const std::string& pairing_code) override {
        return LxpClaimShop(pairing_code);
    }

    void OnEnterWifiConfigMode() override {
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
    }

public:
    CustomBoard() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        Initialize_Expander();
        InitializeSpi();
        InitializeButtons();
        #ifdef CONFIG_BSP_LCD_SIZE_2INCH
            InitializeSt7789Display(); 
        #elif CONFIG_BSP_LCD_SIZE_2_8INCH
            InitializeSt7789Display(); 
        #elif CONFIG_BSP_LCD_SIZE_1_83INCH
            InitializeSt7789Display(); 
        #elif CONFIG_BSP_LCD_SIZE_3_5INCH
            InitializeSt7796Display();
        #endif

        StartBatteryHold();
        InitializeCamera();
        ScheduleOrderBridge();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE);
            return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(CustomBoard);
