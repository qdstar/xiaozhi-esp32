#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/uart.h>

// 邻小铺报单终端（ESP32-D0WD + MAX98357 + INMP441 + WS2812）
// 引脚与 legacy Arduino 固件 lxp-order-terminal 对齐

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// 麦 / 喇叭分总线（Simplex）
#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_13
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_14
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_32

#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_22
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_26
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_25

#define BOOT_BUTTON_GPIO        GPIO_NUM_0

// WS2812 灯环
#define BUILTIN_LED_GPIO        GPIO_NUM_23
#define BUILTIN_LED_NUM         24

// HC-SR501 人体感应：OUT → GPIO34（仅输入），VCC 5V，GND（LXP_LD2450_ENABLE=0 时作回退）
#define LXP_PIR_GPIO            GPIO_NUM_34

// HLK-LD2450：模块 TX → 板子 RX2(GPIO16)；旁路丝印 D4 是另一脚(GPIO4)，不要接错
// 不占用 USB（USB 走 RX0/TX0 = GPIO3/1），烧录与监视器正常
#define LXP_LD2450_ENABLE       1
#define LXP_LD2450_UART_NUM     UART_NUM_1
#define LXP_LD2450_RX_GPIO      GPIO_NUM_16
#define LXP_LD2450_TX_GPIO      GPIO_NUM_NC
#define LXP_LD2450_BAUD         256000
#define LXP_LD2450_WELCOME_MAX_MM  2500  // 进店欢迎最大距离 mm

// 无 OLED：使用 NoDisplay
#define DISPLAY_WIDTH   0
#define DISPLAY_HEIGHT  0

#endif // _BOARD_CONFIG_H_
