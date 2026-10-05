# ESP32-P4 手机监控

> 中文补充（本次新增的注释）：这是工程自带的使用说明，下面是逐段注解。
> 全文 4 个部分：一句话概览 → 手机怎么连 → 怎么编译烧录 → 硬件参数与注意事项。

工程按 ESP-IDF 6.1.0、ESP32-P4 和 MIPI-CSI 摄像头接口配置。启动后 P4 通过板载 ESP32-C6 Wi-Fi 协处理器建立热点，并提供 MJPEG 视频页面。CSI 帧缓冲区需要板载 PSRAM，因此工程已启用 P4 HEX PSRAM（200 MHz）。

> 中文补充：这段用三句话交代了本工程的三个关键前提，每一句都对应一处配置：
> · "ESP-IDF 6.1.0" → `idf_component.yml` 里的 `idf: version: ">=6.1.0"`
> · "板载 ESP32-C6 Wi-Fi 协处理器" → P4 自己**没有 Wi-Fi 射频**，靠 `esp_hosted` 借用 C6
> · "需要板载 PSRAM" → `sdkconfig.defaults` 里的 `CONFIG_SPIRAM=y` + HEX 模式 + 200MHz
> 任意一条不满足，工程都跑不起来。所以这份 README 其实是「环境自检清单」。

## 手机连接

> 中文补充：这是使用者视角的操作步骤，共 3 步。

1. 烧录并启动后，在串口日志中查看 Wi-Fi 信息。
2. 手机连接 `WAVESHARE-P4`，密码 `p4camera8`。
3. 浏览器打开 `http://192.168.4.1`。

> 中文补充：几个知识点：
> · 第 1 步「查看串口日志」是因为程序会打印 `Wi-Fi AP started: SSID=...`，可确认热点真的起来了。
> · 第 3 步的 `192.168.4.1` **不是随便定的**，而是 ESP-IDF 的 SoftAP 默认网关地址
>   （由 `esp_netif_create_default_wifi_ap()` 自动配置），代码里并没有写死这个 IP。
> · 密码 `p4camera8` 是示例值，见文件末尾的提醒。

## 构建与烧录

在 ESP-IDF 6.1.0 PowerShell 环境中执行：

> 中文补充：「在 ESP-IDF 6.1.0 PowerShell 环境中」是个前提条件——必须先运行 Espressif 的
> `export.ps1` 之类的环境脚本，让 `idf.py`、工具链、`IDF_PATH` 都进入当前终端，
> 否则下面三条命令都会报「找不到 idf.py」或「找不到工具链」。

```powershell
cd D:\espidf\jianshi
idf.py reconfigure
idf.py build
idf.py -p COMx flash monitor
```

> 中文补充：四条命令各自的作用：
> · `cd` —— 切到工程根目录（**根目录**，即含 CMakeLists.txt 那一层，不是 main/ 里面）
> · `idf.py reconfigure` —— 重新生成构建系统，并按 `idf_component.yml` 下载缺失的组件。
>   首次构建或改了依赖后需要它；日常可省略。
> · `idf.py build` —— 编译。首次会较慢（要编译整个 IDF + 第三方组件）。
> · `idf.py -p COMx flash monitor` —— 先烧录再打开串口监视器。`COMx` 要换成实际端口号
>   （`.vscode/settings.json` 里当前写的是 COM5）。退出监视器按 `Ctrl + ]`。

此工程按常见 MIPI CSI 接线使用 SCCB SDA GPIO 7、SCL GPIO 8，双 lane、800x640 RAW8。相机驱动组件会尝试自动检测传感器；当前默认格式名针对输出该格式的传感器。如果日志提示未检测到传感器或找不到此格式，需要根据实际摄像头模块型号调整。

> 中文补充：这段实际上是**排错指引**，说的就是「配置与本机硬件不匹配时改哪里」：
> · SCCB 的 SDA/SCL 引脚 → `main/example_config.h` 里
>   `EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO` / `..._SCL_IO`（当前 7 / 8）
> · lane 数与分辨率 800x640 RAW8 → `EXAMPLE_CAM_FORMAT` 这个**格式名**
> · 「自动检测传感器」→ `example_sensor_init()` + `CONFIG_CAMERA_OV5647_AUTO_DETECT_...`
> 判断顺序建议：先确认 I2C 总线能扫到摄像头地址，再看格式名是否匹配，最后看型号是否受支持。

Wi-Fi 热点密码当前为示例密码，请在部署前修改 `main/main.cpp` 中的 `kApPassword`。

> 中文补充：**这是一条真正的安全提醒，不要忽略。** 不改密码等于谁都能连上你的摄像头。
> 修改位置：`main/main.cpp` 顶部的 `constexpr char kApPassword[] = "p4camera8";`。
> 注意两点：① 密码至少 8 位，否则 `WIFI_AUTH_WPA2_PSK` 鉴权会失败（要么加长密码，
> 要么把 `ap_config.ap.authmode` 改成 `WIFI_AUTH_OPEN`，但那样完全没有加密）；
> ② 改密码后手机要「忘记网络」再重连，否则会一直用旧密码认证失败。
