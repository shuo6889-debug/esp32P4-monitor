# 微雪 ESP32-P4 手机监控

这是一个基于 **Waveshare ESP32-P4 Module DEV KIT（A 套餐）** 和 **Waveshare RPi Camera (B)** 的局域网视频监控示例。设备上电后，摄像头采集图像，ESP32-P4 将图像转换为 JPEG，并通过板载 ESP32-C6 建立 Wi-Fi 热点。手机连接该热点后，在浏览器中打开设备地址即可实时查看画面。

当前工程已经在以下硬件和软件组合上验证：

- 主控：ESP32-P4，实机芯片版本 v1.3
- 开发板：Waveshare ESP32-P4 Module DEV KIT，A 套餐（板载 ESP32-C6 Wi-Fi）
- 摄像头：Waveshare RPi Camera (B)，实际检测到 OV5647
- 接口：MIPI CSI-2，两条数据 lane，RAW8
- ESP-IDF：v6.1.0
- Flash：16 MB，DIO，80 MHz
- PSRAM：32 MB HEX PSRAM，200 MHz
- 视频输出：800 x 640，JPEG，MJPEG HTTP 流

## 1. 系统组成和分工

### ESP32-P4

ESP32-P4 是本项目的主处理器，负责：

1. 通过 MIPI CSI 接收摄像头的 RAW8 数据。
2. 使用 ISP 将 RAW8 转换为 RGB565。
3. 使用硬件 JPEG 编码器把 RGB565 压缩成 JPEG。
4. 维护双帧 PSRAM 缓冲区和完成帧队列，避免采集线程被网络发送阻塞。
5. 运行 HTTP 服务器，输出网页和 MJPEG 视频流。

ESP32-P4 本身没有 Wi-Fi 射频，因此不能单独完成无线联网。

### A 套餐中的 ESP32-C6

A 套餐板载 ESP32-C6 作为 Wi-Fi 协处理器。P4 和 C6 之间使用 ESP-Hosted SDIO 通信，P4 侧的网络协议栈通过该链路控制 C6 的 Wi-Fi 射频。工程启动顺序是：

```text
P4 初始化 ESP-Hosted -> 通过 SDIO 连接 C6 -> 启动 C6 Wi-Fi AP -> 启动 HTTP 服务器
```

因此，烧录时应使用 ESP32-P4 的 USB/UART 端口，手机连接的是板载 C6 创建的热点。

## 2. 摄像头说明

Waveshare RPi Camera (B) 是树莓派接口规格的 CSI 摄像头模块，使用 15 针 FFC 排线连接。它的图像传感器实际由启动日志确认是 **OV5647**。本工程使用该传感器的 MIPI 两 lane RAW8 模式，不使用 USB，也不使用传统并口 DVP。

### 摄像头数据流

```text
OV5647
  -> MIPI CSI-2 两 lane / RAW8
  -> ESP32-P4 CSI 控制器
  -> P4 ISP：RAW8 转 RGB565
  -> P4 JPEG 硬件编码器：YUV422，质量 70
  -> MJPEG HTTP 分片
  -> 手机浏览器
```

当前默认采集格式为：

```text
MIPI_2lane_24Minput_RAW8_800x640_50fps
```

这里的 50 fps 是传感器和 CSI 输入模式的能力配置；实际手机显示帧率还会受到 JPEG 编码时间、Wi-Fi 带宽和手机浏览器刷新速度影响。

### 摄像头连接参数

摄像头排线应插入开发板标注的 MIPI CSI 摄像头座，并确认排线金手指方向与座子锁扣方向一致。不要带电插拔排线。

| 信号 | P4 工程配置 | 说明 |
| --- | ---: | --- |
| SCCB/I2C SDA | GPIO 7 | 配置 OV5647 寄存器 |
| SCCB/I2C SCL | GPIO 8 | 配置 OV5647 寄存器 |
| MIPI 数据 lane | 2 lane | CSI-2 数据通道 |
| MIPI lane 速率 | 200 Mbps | `main/example_config.h` |
| CSI 输入 | RAW8 | P4 CSI 接收格式 |
| ISP 输出 | RGB565 | 供 JPEG 编码器使用 |
| 图像尺寸 | 800 x 640 | 当前工程固定尺寸 |

摄像头供电、时钟和复位由开发板的 MIPI 摄像头接口提供。工程中没有另外定义 XCLK、PWDN 或 RESET GPIO；如果使用的不是 RPi Camera (B)，需要根据新摄像头原理图修改传感器驱动配置。

## 3. P4-C6 SDIO 连接

A 套餐的板载 Wi-Fi 协处理器由 ESP-Hosted 使用 4-bit SDIO 连接。以下是本工程对应的 P4 侧信号：

| SDIO 信号 | GPIO | 方向/用途 |
| --- | ---: | --- |
| CLK | GPIO 18 | SDIO 时钟，40 MHz |
| CMD | GPIO 19 | 命令线 |
| D0 | GPIO 14 | 数据线 0 |
| D1 | GPIO 15 | 数据线 1 |
| D2 | GPIO 16 | 数据线 2 |
| D3 | GPIO 17 | 数据线 3 |
| C6 RESET | GPIO 54 | 控制 C6 复位 |

这些信号由开发板硬件连接，通常不需要用户外接跳线。不要把 GPIO 14-19 当作普通 GPIO 使用，否则会破坏 Wi-Fi 通信。

## 4. 手机查看步骤

1. 给开发板供电，并将 USB/UART 接到电脑。
2. 烧录工程后打开串口监视器，等待出现以下日志：

   ```text
   Detected Camera sensor PID=0x5647
   Camera stream active: frame=100, jpeg=... bytes
   Wi-Fi AP started: SSID=WAVESHARE-P4, channel=6
   HTTP server listening on port 80
   ```

3. 手机 Wi-Fi 连接：

   - SSID：`WAVESHARE-P4`
   - 密码：`p4camera8`

4. 关闭手机的移动数据或暂时忽略“该 Wi-Fi 无互联网”的提示，避免手机自动切换到移动网络。
5. 手机浏览器访问：[`http://192.168.4.1`](http://192.168.4.1)

网页根路径 `/` 会显示视频画面，视频接口为 `/stream`。如果需要在其他程序中读取视频，可直接使用：

```text
http://192.168.4.1/stream
```

该接口是 `multipart/x-mixed-replace` 格式的 MJPEG 流，不是 H.264，也不是 RTSP。

## 5. 工程目录

```text
jianshi/
├─ main/
│  ├─ main.cpp              # 摄像头采集、JPEG、Wi-Fi AP、HTTP 服务
│  ├─ example_config.h      # SCCB、MIPI lane 速率和摄像头格式
│  ├─ example_sensor_init.* # ESP-IDF 摄像头传感器初始化组件
│  └─ idf_component.yml     # 组件依赖
├─ sdkconfig.defaults       # P4、OV5647、PSRAM、Flash 默认配置
├─ CMakeLists.txt
└─ README.md
```

修改摄像头分辨率或格式时，应同时检查 `main.cpp` 中的 `kWidth`、`kHeight`、CSI 配置、ISP 配置和 JPEG 配置，不能只改一个宏。

## 6. ESP-IDF 构建和烧录

建议使用 ESP-IDF v6.1.0 PowerShell 或 VS Code 的 ESP-IDF 扩展。PowerShell 示例：

```powershell
$env:IDF_PATH = 'D:\ESP_IDF\v6.1\esp-idf'
$env:ESP_IDF_VERSION = 'v6.1'
$env:IDF_PYTHON_ENV_PATH = 'C:\Espressif\tools\python\v6.1\venv'
cd D:\espidf\jianshi

idf.py reconfigure
idf.py build
idf.py -p COM5 flash monitor
```

把 `COM5` 换成电脑实际识别到的端口。也可以分别执行：

```powershell
idf.py -p COM5 flash
idf.py -p COM5 monitor
```

首次切换芯片版本或分区配置后，建议删除旧的 `build` 目录再重新配置：

```powershell
Remove-Item -Recurse -Force .\build
idf.py reconfigure
idf.py build
```

如果使用 VS Code ESP-IDF 扩展，目标芯片必须选择 `esp32p4`，端口选择连接 P4 的 COM 口。不要把 C6 的下载端口当作 P4 固件烧录端口。

## 7. 正常启动日志判定

以下信息表示摄像头、内存、Wi-Fi 和 HTTP 服务均已启动：

```text
chip revision: v1.3
SPI Flash Size : 16MB
Detected Camera sensor PID=0x5647
Format in use:MIPI_2lane_24Minput_RAW8_800x640_50fps
Camera stream active: frame=100, jpeg=... bytes
Wi-Fi AP started: SSID=WAVESHARE-P4, channel=6
HTTP server listening on port 80
```

`frame=100`、`frame=200` 等日志持续增长，说明 CSI 回调、双缓冲和 JPEG 编码正在连续工作。出现 `main_task: Returned from app_main()` 通常表示 `app_main()` 在初始化失败后返回，需要查看它之前的第一条 `E (...)` 日志。

## 8. 常见问题

### `requires chip revision in range [v3.1 - v3.99]`

这是旧的 `sdkconfig` 或旧 bootloader 按更高芯片版本生成，而实机是 ESP32-P4 v1.3。删除 `build`，确认 `CONFIG_ESP32P4_REV_MIN` 不高于 v1.x，再重新 `reconfigure` 和 `build`。不要长期使用 `--force` 强行烧录不匹配的 bootloader。

### `CSI controller init failed: ESP_ERR_NO_MEM`

CSI 备份缓冲区和帧缓冲区需要 PSRAM。确认已启用 32 MB HEX PSRAM，并且 `sdkconfig` 中包含 `CONFIG_SPIRAM=y`、`CONFIG_SPIRAM_MODE_HEX=y`、`CONFIG_SPIRAM_SPEED_200M=y`。也要确认 Flash/PSRAM 模式与 A 套餐硬件一致。

### `no on_trans_finished callback registered`

CSI 启动前必须注册 `on_get_new_trans` 和 `on_trans_finished` 回调。本工程已在 `initialize_camera()` 中注册，并用完成帧队列交给 JPEG 任务；如果修改采集流程，不能省略这两个回调。

### 只能检测到摄像头，画面不连续

确认 `Camera stream active` 的帧号持续增加。若只出现第一帧，通常是重复调用 `esp_cam_ctlr_receive()` 或没有从 CSI 回调接收完成帧。本工程使用两个 PSRAM DMA 缓冲区和 FreeRTOS 队列处理连续帧。

### 出现 `Version mismatch: Host [2.12.0] > Co-proc [0.0.0]`

这是 ESP-Hosted 读取到板载 ESP32-C6 出厂协处理器固件版本为 `0.0.0` 的提示。当前工程已经实测可以完成 SDIO 建链、启动热点和提供 HTTP 服务；只要后续出现 `Wi-Fi AP started`，该提示不会阻止本项目使用。若需要消除提示，应单独升级 C6 的 ESP-Hosted 协处理器固件，并保证它与 P4 侧组件版本匹配。

### 手机连上热点但打不开页面

确认串口已经打印 `HTTP server listening on port 80`，手机地址栏输入完整的 `http://192.168.4.1`，不要使用 `https`。暂时关闭移动数据和 VPN，并确认手机没有连接到同名的其他热点。

## 9. 修改热点密码

当前密码写在 `main/main.cpp`：

```cpp
constexpr char kApSsid[] = "WAVESHARE-P4";
constexpr char kApPassword[] = "p4camera8";
```

修改后重新编译烧录。WPA2 密码至少需要 8 个字符；修改 SSID 后，手机需要重新选择新的热点名称。

## 10. 当前限制

- 视频是局域网 MJPEG，没有云端访问、账号认证和 HTTPS。
- ESP32-P4 通过板载 C6 提供 AP，手机必须处于该热点覆盖范围内。
- 当前分辨率固定为 800 x 640，JPEG 质量固定为 70。
- 默认最多允许 4 个 Wi-Fi 客户端；多个手机同时观看会增加 C6 Wi-Fi 和 P4 HTTP 发送负载。
- `/stream` 是 MJPEG，不兼容只支持 RTSP/H.264 的监控软件。
- 设备没有自动保存录像功能；如需录像，应在手机或电脑端保存 MJPEG 流。

## 11. 相关资料

- [Waveshare ESP32-P4 Module DEV KIT](https://docs.waveshare.net/ESP32-P4-Module-DEV-KIT/)
- [Waveshare RPi Camera (B)](https://www.waveshare.net/wiki/RPi_Camera_%28B%29)
- [ESP-IDF 官方文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/)
