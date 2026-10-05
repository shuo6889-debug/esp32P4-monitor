# ESP32-P4 手机监控

工程按 ESP-IDF 6.1.0、ESP32-P4 和 MIPI-CSI 摄像头接口配置。启动后 P4 通过板载 ESP32-C6 Wi-Fi 协处理器建立热点，并提供 MJPEG 视频页面。CSI 帧缓冲区需要板载 PSRAM，因此工程已启用 P4 HEX PSRAM（200 MHz）。

## 手机连接

1. 烧录并启动后，在串口日志中查看 Wi-Fi 信息。
2. 手机连接 `WAVESHARE-P4`，密码 `p4camera8`。
3. 浏览器打开 `http://192.168.4.1`。

## 构建与烧录

在 ESP-IDF 6.1.0 PowerShell 环境中执行：

```powershell
cd D:\espidf\jianshi
idf.py reconfigure
idf.py build
idf.py -p COMx flash monitor
```

此工程按常见 MIPI CSI 接线使用 SCCB SDA GPIO 7、SCL GPIO 8，双 lane、800x640 RAW8。相机驱动组件会尝试自动检测传感器；当前默认格式名针对输出该格式的传感器。如果日志提示未检测到传感器或找不到此格式，需要根据实际摄像头模块型号调整。

Wi-Fi 热点密码当前为示例密码，请在部署前修改 `main/main.cpp` 中的 `kApPassword`。
