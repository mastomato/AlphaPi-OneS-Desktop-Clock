# 第三方来源说明

根目录 `LICENSE` 为上游 [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的 MIT 许可证，保留原版权。

本工程固件包含以下来源的内容：

- **xiaozhi-esp32**：基础框架、音频服务、协议与 OTA，MIT 许可（见根目录 LICENSE）。
- **LVGL**：图形库，MIT 许可。
- **ESP-IDF / ESP Component Manager 组件**（含 `78/esp-wifi-connect`、`esp-ml307` 等）：以各自许可证为准，由 IDF Component Manager 获取，未打包进本仓库。
- **AlphaPi-Radio 2.3.2 资料**：收音机 `radio_player` 及半双工 UART 音频链路的实现参考了该资料源码。资料包未附带独立许可证，公开可下载不等于已有开源再分发授权；本仓库不对其重新授予 MIT 许可。
- **字体**：时钟页数字 / 中文子集字体由 `lv_font_conv` 从 Segoe UI、思源等字体生成，字体版权归各自权利人所有。

本仓库**未打包**原厂商业固件、用户照片、NVS 配网数据或电池分析转储。
刷机包中的 `nvs` 分区不在六段镜像之列，不含任何个人配置。
