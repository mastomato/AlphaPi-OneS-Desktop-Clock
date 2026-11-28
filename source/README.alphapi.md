# AlphaPi One S 桌面时钟 — 小智侧源码定制说明

这是 `xiaozhi.bin`（ota_0 槽：小智语音 + 桌面时钟 + 中文天气 + 三合一设置页）的源码。
基于 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（上游 README 见 [README.md](README.md)），
针对 AlphaPi One S（ESP32-S2R2 / 160×128 ST7735 / N32 UART 音频协处理器 / 4 键输入）做了板级移植。

> **源码范围提醒**：本源码树只覆盖**小智槽**。收音机槽（`radio.bin`，ota_1）是基于上游
> 官方固件做**二进制补丁**得到的（改色 + 长按 D 切槽逻辑），没有可编译源码，交付包中直接提供
> `.bin`。如需改收音机配色，用 `firmware/v16/extras/patch_radio_m2*.py` 在官方 bin 上打补丁，
> 不要重编（重编会丢切槽逻辑）。

## 目录说明

```
source/
├── main/                        # 应用源码（板级在 main/boards/alphapi/alphapi-one-s/）
│   ├── boards/alphapi/alphapi-one-s/
│   │   ├── alphapi_one_s.cc / .h       板级：N32 UART 音频、SPI 屏、4 键、双固件切槽
│   │   ├── alphapi_one_audio_codec.*   N32 音频 codec（24kHz/8bit PCM over UART1）
│   │   ├── alphapi_idle_clock.cc       深色时钟页（TextCache 抗卡顿、独立构建任务）
│   │   ├── alphapi_weather.cc          三合一设置页 + 天气 httpd（栈在 PSRAM、低优先级）
│   │   └── config.h                    板级配置（I2S 关闭、UART 引脚等）
│   ├── display/                        LVGL 显示层（lcd_display.cc 锁 light/dark 双主题深色）
│   └── audio/audio_service.cc          音频服务（按 S2 内存调整）
├── managed_components/
│   └── 78__esp-wifi-connect/           # ★ 本项目定制版，勿用官方原版覆盖
│       └── ssid_manager.cc             # 配网保存时同步写入收音机 NVS namespace，实现"配一次网"
├── sdkconfig / sdkconfig.defaults*     # 已定稿配置（SPIRAM_MALLOC_ALWAYSINTERNAL=16384 等）
├── dependencies.lock                   # 组件版本锁（esp-idf 6.0.2）
└── partitions/ scripts/ docs/ docker/  # 分区表与辅助脚本
```

## 相对上游的关键改动

1. **深色主题根治白底**：`lcd_display.cc::SetTheme()` 把 light 与 dark 两个主题的
   背景/文字/气泡色都锁成 `#0E1420` 系列——资源包 `index.json` 的 `skin.light` 原本是白底，
   设备默认又是 light 主题，只锁 dark 会导致刷机后小智界面仍是白的。
2. **卡顿修复**：`alphapi_idle_clock.cc` 移除 LVGL timer 内的 `lv_refr_now` 重入刷新，
   12 个 label 加文本缓存，时钟构建移到独立 FreeRTOS 任务（原 esp_timer 栈不足）。
3. **配网只配一次**：定制的 `ssid_manager.cc::SaveToNvs()` 在保存时把默认 SSID/密码
   同步写入 NVS namespace `alphapi_radio`（收音机固件认的 namespace），切槽后收音机直接联网。
4. **三合一设置页**：`alphapi_weather.cc` 把配网 / 天气位置 / 网络收音机合并为一个常驻 httpd
   页面（`task_caps=SPIRAM`、`task_priority=1`、`max_open_sockets=3`，避免抢占音频输出任务）。
5. **音频内存布局**：`SPIRAM_MALLOC_ALWAYSINTERNAL=16384`、mbedTLS 缓冲放 PSRAM——
   PCM 帧与 Opus 解码状态留在内部 RAM，避免"讲几个字就没声"的断流。
6. **WiFi 历史记录与会话失败恢复**：`ssid_manager.cc` / `application.cc` 的断流恢复改动。

## 编译

需要 ESP-IDF **6.0.2**（`$IDF_PATH` 指向 `esp-idf-v6.0.2`）：

```bash
# 1) 首次 reconfigure 会按 dependencies.lock 拉取官方组件
idf.py reconfigure

# 2) ★ 用本源码自带的定制组件覆盖拉下来的官方版（关键是 ssid_manager.cc 的配网共享改动）：
#    把 source/managed_components/78__esp-wifi-connect/ 整个覆盖到
#    <工程>/managed_components/78__esp-wifi-connect/

# 3) 编译（板级已在 main/boards/alphapi/ 中，target 为 esp32s2）
idf.py set-target esp32s2
idf.py build

# 产物 build/xiaozhi.bin 单段刷 0x20000：
python -m esptool --chip esp32s2 -p COMx -b 115200 --before default-reset --after hard-reset write-flash 0x20000 build/xiaozhi.bin
```

> `sdkconfig` 已定稿，**不要**随意改动内存相关项——阈值（16384 / 1024 / 8192）在调试中反复
> 验证过，改错会直接导致音频断流或 TLS 握手失败，详见顶层 `CHANGELOG.md` 与
> `firmware/v16/README.md` 的排障记录。

## 许可

基于 xiaozhi-esp32（MIT），保留上游版权声明。见仓库顶层 `LICENSE` 与 `THIRD_PARTY_NOTICES.md`。
