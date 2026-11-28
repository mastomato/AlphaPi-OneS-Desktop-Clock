#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

namespace radio {

// ---- Display (ST7735 128x160 panel, used landscape 160x128) ----
constexpr int kDisplayWidth = 160;
constexpr int kDisplayHeight = 128;
constexpr spi_host_device_t kDisplaySpiHost = SPI2_HOST;
constexpr gpio_num_t kDisplayMosi = GPIO_NUM_38;
constexpr gpio_num_t kDisplayClock = GPIO_NUM_37;
constexpr gpio_num_t kDisplayCs = GPIO_NUM_34;
constexpr gpio_num_t kDisplayDc = GPIO_NUM_36;
constexpr gpio_num_t kDisplayReset = GPIO_NUM_35;
constexpr gpio_num_t kDisplayBacklight = GPIO_NUM_33;
constexpr int kDisplayClockHz = 26 * 1000 * 1000;
constexpr bool kDisplaySwapXy = true;
constexpr bool kDisplayMirrorX = true;
constexpr bool kDisplayMirrorY = false;
constexpr bool kDisplayInvert = false;

// ---- Audio co-processor over UART ----
constexpr uart_port_t kAudioUart = UART_NUM_1;
constexpr int kAudioBaudRate = 460800;
constexpr gpio_num_t kAudioTx = GPIO_NUM_3;
constexpr gpio_num_t kAudioRx = GPIO_NUM_0;
constexpr int kOutputSampleRate = 24000;

// ---- Keys ----
constexpr gpio_num_t kButtonA = GPIO_NUM_13;
constexpr gpio_num_t kButtonB = GPIO_NUM_12;
constexpr gpio_num_t kButtonC = GPIO_NUM_11;
constexpr gpio_num_t kButtonD = GPIO_NUM_10;

}  // namespace radio
