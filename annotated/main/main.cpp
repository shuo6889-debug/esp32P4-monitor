// =============================================================================
//  ESP32-P4 手机监控（MJPEG 网络摄像头）—— 带注释阅读版
// =============================================================================
//  这不是可直接编译的原文件，只是原 main/main.cpp 的「加注释副本」。
//  代码本身与原文件逐字一致，所有中文说明均为新增注释。
//  想校验一致性：见 annotated/README-先读我.md 里的 diff 命令。
//
//  ---------------------------------------------------------------------------
//  这个程序做什么
//  ---------------------------------------------------------------------------
//  把 ESP32-P4 开发板变成一个「自带 Wi-Fi 热点的网络摄像头」：
//  上电后摄像头持续采集画面 → 压缩成 JPEG → 通过 HTTP 把一串 JPEG 连续推给
//  手机浏览器，浏览器里就看到了实时视频（约等于 10~25 fps，取决于分辨率）。
//
//  ---------------------------------------------------------------------------
//  完整数据流水线（理解这条线，就理解了这个项目 80%）
//  ---------------------------------------------------------------------------
//
//    OV5647 摄像头模组
//        │  ① MIPI-CSI 物理接口：2 条数据 lane，每 lane 200 Mbps，24MHz 输入时钟
//        ▼
//    ESP32-P4 CSI 控制器        （CPU 不参与搬运）
//        │  ② 硬件把串行像素流还原成 RAW8 像素，DMA 直接写入 PSRAM 缓冲区
//        ▼
//    PSRAM 中的帧缓冲区          （800×640×2 = 1 024 000 字节/帧）
//        │  ③ ISP 图像信号处理器：RAW8 → RGB565（原地转换，写回同一块内存）
//        ▼
//    同一块缓冲区（此时内容是 RGB565）
//        │  ④ JPEG 硬件编码器：RGB565 → JPEG，质量 70
//        ▼
//    JPEG 缓冲区（s_jpeg_buffer）
//        │  ⑤ HTTP 分块传输，multipart/x-mixed-replace
//        ▼
//    手机浏览器 <img src="/stream">  → 自动连续刷新，形成视频
//
//    Wi-Fi 链路是另一条独立的旁路：
//    ESP32-P4 本身【没有】Wi-Fi 射频，靠板上另一颗 ESP32-C6 芯片当「网卡」，
//    两者用 SDIO 相连，软件上由 ESP-Hosted 框架打通（见 start_wifi_ap()）。
//
//  ---------------------------------------------------------------------------
//  并发模型（谁在什么时候跑）
//  ---------------------------------------------------------------------------
//    · CSI 中断（ISR）      —— camera_get_buffer / camera_trans_finished
//                              在中断上下文里跑，只做「发缓冲区、丢队列」两件事
//    · camera_capture 任务  —— camera_task()，优先级 5，负责等帧 + JPEG 编码
//    · httpd 任务           —— ESP-IDF 的 HTTP 服务器内部只创建【一个】任务
//                              （名字就叫 "httpd"），它用 select() 轮询所有
//                              连接，然后依次调用各请求的处理函数。
//                              这条事实有个重要后果：stream_handler() 在连接
//                              存续期间一直不返回，于是它把整个 httpd 任务独占了
//                              ——详见 stream_handler() 结尾的「单客户端限制」。
//    · app_main             —— 主任务，只负责「开机装配」，装配完就返回
//
//    两个任务之间靠两样东西通信：
//      s_completed_frames —— FreeRTOS 队列，ISR 把「哪块缓冲区拍满了」丢进来
//      s_frame_mutex      —— 互斥锁，保护「JPEG 缓冲区 + 帧号」这一对共享数据
// =============================================================================

#include <cstdio>      // snprintf：拼 HTTP 分块头
#include <cstddef>     // size_t
#include <cstdlib>     // 基础工具
#include <cstring>     // memcpy / strncpy / strlen
#include <inttypes.h>  // PRIu32：printf 打印 uint32_t 的可移植写法

// ---- 驱动层头文件：本项目直接操作的外设 ----
#include "driver/i2c_master.h"     // I2C 主机（本项目中用于 SCCB 控制摄像头）
#include "driver/isp.h"            // ISP 图像信号处理器驱动
#include "driver/jpeg_encode.h"    // JPEG 硬件编码器驱动
#include "esp_cam_ctlr.h"          // 摄像头控制器通用接口
#include "esp_cam_ctlr_csi.h"      // 摄像头控制器：MIPI-CSI 专用接口
#include "esp_check.h"             // ESP_RETURN_ON_ERROR / ESP_RETURN_ON_FALSE 宏
#include "esp_attr.h"              // IRAM_ATTR：把函数放进内部 RAM，中断里才能安全调用
#include "esp_event.h"             // 事件循环（Wi-Fi 协议栈依赖它）
#include "esp_http_server.h"       // HTTP 服务器
#include "esp_hosted.h"            // ESP-Hosted：让 P4 借用 C6 的 Wi-Fi
#include "esp_ldo_regulator.h"     // 片内 LDO，给 MIPI PHY 供电
#include "esp_log.h"               // ESP_LOGI/W/E：串口日志
#include "esp_netif.h"             // 网络接口抽象层（TCP/IP 与 Wi-Fi 之间的桥）
#include "esp_wifi.h"              // Wi-Fi 驱动
#include "esp_wifi_default.h"      // esp_netif_create_default_wifi_ap()
#include "esp_cache.h"             // esp_cache_msync：CPU 缓存与 DMA 的一致性维护
#include "esp_heap_caps.h"         // heap_caps_malloc：按内存类型申请（如只申请 PSRAM）
#include "nvs_flash.h"             // NVS 非易失存储（Wi-Fi 驱动需要它存校准数据）
#include "freertos/FreeRTOS.h"     // FreeRTOS 基础
#include "freertos/queue.h"        // xQueueCreate/xQueueSendFromISR/xQueueReceive
#include "freertos/semphr.h"       // 互斥锁
#include "freertos/task.h"         // xTaskCreate / vTaskDelay
#include "example_sensor_init.h"   // 官方例程组件：自动探测并初始化摄像头传感器
#include "example_config.h"        // 本工程的接线与格式配置

// 匿名命名空间：这里所有符号只在本文件可见，相当于 C 里的 static，
// 避免与其它组件重名（C++ 工程的好习惯）。
namespace {

// -----------------------------------------------------------------------------
// 一、编译期常量：把「魔法数字」集中到一处，改分辨率/密码时只动这里
// -----------------------------------------------------------------------------

constexpr int kWidth = 800;    // 采集宽度（必须与摄像头模组支持的格式一致）
constexpr int kHeight = 640;   // 采集高度

// 单帧缓冲区字节数 = 宽 × 高 × 2。
// 为什么要乘 2（而不是 RAW8 直觉上的 1）？
//   · CSI 输出端以 16 位为单位搬运 RAW8 像素，缓冲区按 2 字节/像素摆放；
//   · 更关键的是：ISP 之后这块内存要装 RGB565，而 RGB565 正是 2 字节/像素。
//   所以同一块缓冲区「前半段旅程装 RAW8、后半段旅程装 RGB565」，大小刚好够用，
//   这就是 ISP 能原地（in-place）转换、不需要再开一块 RGB 缓冲的原因。
constexpr size_t kRawFrameBytes = kWidth * kHeight * 2;   // 1 024 000 字节

// JPEG 输出缓冲区：留出原图 + 64KB 余量。
// JPEG 是变长压缩，头部开销、色度子采样边界等情况可能让输出略大于最坏估计，
// 多留一点可以避免极端画面下编码失败。
constexpr size_t kJpegBufferBytes = kRawFrameBytes + 64 * 1024;

constexpr char kApSsid[] = "WAVESHARE-P4";  // 手机要连的热点名
constexpr char kApPassword[] = "p4camera8"; // 热点密码（至少 8 位才能用 WPA2）

// MJPEG 流的分隔边界名。浏览器按这个字符串切分「第几帧」。
// Content-Type: multipart/x-mixed-replace; boundary=frame 必须与它一致。
constexpr char kBoundary[] = "frame";

const char *TAG = "jianshi";  // 日志前缀，串口里看到 "I (1234) jianshi:" 就是本程序

// -----------------------------------------------------------------------------
// 二、全局状态：整个程序的所有「活动部件」
//    （嵌入式程序常用这种「文件级全局变量」风格：生命周期与程序等长，省去传递句柄）
// -----------------------------------------------------------------------------

esp_cam_ctlr_handle_t s_camera = nullptr;  // 摄像头控制器句柄（初始化后一直用）

// 双缓冲：为什么需要 2 块而不是 1 块？
//   如果只有 1 块，DMA 拍下一帧时会直接覆盖 CPU/JPEG 正在读的那一帧，
//   画面就会出现「上半部分是上一帧、下半部分是这一帧」的撕裂。
//   两块缓冲轮换，就能做到「一块在拍、一块在处理」。
constexpr size_t kCaptureBufferCount = 2;
void *s_capture_buffers[kCaptureBufferCount] = {};
size_t s_next_capture_buffer = 0;   // 轮换下标：0 → 1 → 0 → 1 …

// ISR 与任务之间的交接队列：元素是「已经拍满的那块缓冲区的地址」。
QueueHandle_t s_completed_frames = nullptr;

jpeg_encoder_handle_t s_jpeg_encoder = nullptr;  // JPEG 硬件编码器句柄
uint8_t *s_jpeg_buffer = nullptr;                // JPEG 输出缓冲区（下面 3 个变量描述它）
size_t s_jpeg_capacity = 0;                      // 容量（字节）
size_t s_jpeg_size = 0;                          // 当前有效 JPEG 数据的实际长度

uint32_t s_frame_number = 0;      // 帧计数器，也用作「有没有新帧」的判断依据
SemaphoreHandle_t s_frame_mutex = nullptr;  // 保护 s_jpeg_buffer/s_jpeg_size/s_frame_number

// =============================================================================
// 三、CSI 中断回调：摄像头的「心跳」
// =============================================================================

// 【回调 1】硬件要拍下一帧了，问你要一块缓冲区。
// 注意 IRAM_ATTR：这个函数在中断上下文执行，必须常驻内部 RAM，
// 否则 flash cache 一旦被别的操作占用就会崩（中断里不能读 flash）。
bool IRAM_ATTR camera_get_buffer(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)user_data;  // 用不到，显式丢弃以避免编译警告

    // 把当前这块缓冲区交给 DMA，并告诉它「最多往里写这么多字节」
    trans->buffer = s_capture_buffers[s_next_capture_buffer];
    trans->buflen = kRawFrameBytes;

    // 立刻切到另一块，这样 DMA 拍当前帧的同时，另一块可以被上层处理
    s_next_capture_buffer = (s_next_capture_buffer + 1) % kCaptureBufferCount;

    // 返回 false = 「我没有唤醒更高优先级任务，不需要立刻切换上下文」。
    // 这里只是发块内存，没有任何 FreeRTOS 调度请求，所以固定返回 false。
    return false;
}

// 【回调 2】一帧拍完了。
// 这里只做一件事：把「这块缓冲区已经拍满」的消息丢进队列，然后马上返回。
// 中断里绝不做耗时操作（更不能做 JPEG 编码、memcpy 大块数据、打印日志）。
bool IRAM_ATTR camera_trans_finished(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *)
{
    // 防御性检查：队列还没建好时（理论上不会）直接忽略，避免在中断里解引用空指针
    if (s_completed_frames == nullptr || trans == nullptr || trans->buffer == nullptr) {
        return false;
    }

    void *completed_frame = trans->buffer;   // 局部变量：队列发送要的是它的地址
    BaseType_t high_task_woken = pdFALSE;    // 出参：是否唤醒了更高优先级任务

    // 必须用 FromISR 版本才能在中断里调用；非阻塞，队列满了就丢弃这一帧
    // （丢帧比阻塞中断安全得多——中断被卡住会让整个系统失速）
    (void)xQueueSendFromISR(s_completed_frames, &completed_frame, &high_task_woken);

    // 告诉 FreeRTOS：如果刚才唤醒了更高优先级任务（比如 camera_task），
    // 请在退出中断时立刻做一次任务切换，让编码尽快开始。
    return high_task_woken == pdTRUE;
}

// =============================================================================
// 四、初始化摄像头链路：整个工程最核心的一段
// =============================================================================
// 顺序不能乱，每一步都为下一步做铺垫：
//   ① 给 MIPI PHY 供电 → ② 通过 SCCB 配置传感器 → ③ 建 CSI 控制器
//   → ④ 分配帧缓冲 + 队列 → ⑤ 注册回调 → ⑥ 使能 CSI
//   → ⑦ 建 ISP（RAW8→RGB565）→ ⑧ 建 JPEG 编码器 → ⑨ 启动采集
esp_err_t initialize_camera()
{
    // ---- ① MIPI 电源 ----
    // MIPI-CSI 的物理层（PHY）需要独立供电，P4 用片内 LDO 通道 3 输出 2.5V。
    // 漏掉这一步的典型症状：传感器能配置成功，但一帧都收不到。
    esp_ldo_channel_config_t ldo_config = {};
    ldo_config.chan_id = 3;         // 用 LDO 通道 3（板级设计决定，查原理图可得）
    ldo_config.voltage_mv = 2500;   // 2.5V
    esp_ldo_channel_handle_t mipi_ldo = nullptr;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &mipi_ldo), TAG, "MIPI LDO init failed");

    // ---- ② 探测并初始化摄像头传感器 ----
    // sensor_init 是官方例程提供的通用组件：它会通过 SCCB（本质是 I2C）逐个
    // 尝试读各种传感器的 ID 寄存器，认出是 OV5647 还是别的型号，然后写入
    // 该型号在这个分辨率下的整套寄存器配置。
    example_sensor_config_t sensor_config = {};
    sensor_config.i2c_port_num = I2C_NUM_0;                          // 用 I2C0 总线
    sensor_config.i2c_sda_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO; // SDA = GPIO7
    sensor_config.i2c_scl_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO; // SCL = GPIO8
    sensor_config.reset_pin = -1;   // -1 表示该模组没有独立的复位脚
    sensor_config.pwdn_pin = -1;    // -1 表示没有 power-down 脚
    sensor_config.xclk_pin = -1;    // -1 表示不用 P4 输出主时钟，模组自带晶振
    sensor_config.port = ESP_CAM_SENSOR_MIPI_CSI;      // 接口类型：MIPI-CSI
    sensor_config.format_name = EXAMPLE_CAM_FORMAT;    // 目标格式，见 example_config.h

    example_sensor_handle_t sensor = {};
    example_sensor_init(&sensor_config, &sensor);

    // 组件内部靠 sccb_handle 判断是否探测成功。
    // 这里【刻意没检查返回值】而是检查句柄：即使探测失败，也走到同样的分支，
    // 用一句明确的日志告诉用户「大概率是型号不支持或接线不对」。
    if (sensor.sccb_handle == nullptr) {
        ESP_LOGE(TAG, "No supported MIPI camera was detected. Check the camera module and SCCB wiring.");
        return ESP_ERR_NOT_FOUND;
    }

    // ---- ③ 创建 CSI 控制器 ----
    // 一句话理解：CSI 控制器 = 「把 MIPI 串行像素流变成内存里的像素数组」的硬件搬运工。
    esp_cam_ctlr_csi_config_t csi_config = {};
    csi_config.ctlr_id = 0;                    // P4 上 CSI 主机编号，一个就够
    csi_config.h_res = kWidth;                 // 分辨率必须与传感器输出一致
    csi_config.v_res = kHeight;
    csi_config.lane_bit_rate_mbps = EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS;  // 200 Mbps
    csi_config.input_data_color_type = CAM_CTLR_COLOR_RAW8;   // 摄像头送来的是 RAW8
    // ESP32-P4 revision v1.x does not support color conversion in the CSI
    // bridge. Keep CSI in RAW8 bypass mode; the ISP below converts to RGB565.
    // 中文补充：老版本 P4 的 CSI 桥不支持直接在硬件里做颜色转换，
    // 所以这里输出必须保持 RAW8（直通），把 RGB565 转换这件事交给 ISP 干。
    // 如果误把 output 改成 RGB565，在这批芯片上会拿到错误图像或直接不工作。
    csi_config.output_data_color_type = CAM_CTLR_COLOR_RAW8;
    csi_config.data_lane_num = 2;              // 用了 2 条数据 lane
    csi_config.queue_items = 1;                // 控制器内部帧队列深度
    ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi_config, &s_camera), TAG, "CSI controller init failed");

    // ---- ④ 分配帧缓冲区 + 建立交接队列 ----
    for (size_t i = 0; i < kCaptureBufferCount; ++i) {
        // MALLOC_CAP_DMA：必须是 DMA 能访问的内存（否则硬件无法直接写入）
        // MALLOC_CAP_SPIRAM：放进 PSRAM，因为两帧共 2MB，内部 RAM 根本装不下
        // 注意：一定要用 esp_cam_ctlr_alloc_buffer 而不是普通 malloc，
        // 因为摄像头驱动对缓冲区的对齐、缓存属性有额外要求。
        s_capture_buffers[i] = esp_cam_ctlr_alloc_buffer(s_camera, kRawFrameBytes,
                                                          MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_capture_buffers[i] != nullptr, ESP_ERR_NO_MEM, TAG,
                           "Camera frame allocation failed");
    }

    // 队列最多存 2 个指针（和缓冲区数量一致），存不下时 ISR 会丢弃该帧
    s_completed_frames = xQueueCreate(kCaptureBufferCount, sizeof(void *));
    ESP_RETURN_ON_FALSE(s_completed_frames != nullptr, ESP_ERR_NO_MEM, TAG,
                       "Camera frame queue allocation failed");

    // ---- ⑤ 注册中断回调 ----
    esp_cam_ctlr_evt_cbs_t callbacks = {};
    callbacks.on_get_new_trans = camera_get_buffer;      // 「要新缓冲区」时调用
    callbacks.on_trans_finished = camera_trans_finished; // 「一帧拍完」时调用
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(s_camera, &callbacks, nullptr), TAG, "Camera callbacks failed");

    // 使能控制器：硬件准备就绪，但还没开始拍（start 才真正开始）
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(s_camera), TAG, "Camera enable failed");

    // ---- ⑥ 建立 ISP 图像信号处理器：RAW8 → RGB565 ----
    // 传感器直接吐出来的 RAW8 是「拜耳阵列」原始数据，每个像素只有一种颜色，
    // 不能直接当图片看。ISP 要做去马赛克、白平衡、RGB 转换，输出真正的彩色图。
    esp_isp_processor_cfg_t isp_config = {};
    isp_config.clk_hz = 80000000;                     // ISP 工作时钟 80MHz
    isp_config.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;  // 数据来源：CSI
    isp_config.input_data_color_type = ISP_COLOR_RAW8;         // 输入 RAW8
    isp_config.output_data_color_type = ISP_COLOR_RGB565;      // 输出 RGB565
    isp_config.has_line_start_packet = false;         // 这路 CSI 信号没有行起止包
    isp_config.has_line_end_packet = false;           // 也没有行结束包
    isp_config.h_res = kWidth;                        // 分辨率必须与 CSI 完全一致
    isp_config.v_res = kHeight;
    isp_proc_handle_t isp = nullptr;
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_config, &isp), TAG, "ISP init failed");
    ESP_RETURN_ON_ERROR(esp_isp_enable(isp), TAG, "ISP enable failed");
    // 说明：ISP 的输入/输出都指向同一块帧缓冲区（原地转换），所以这里不需要
    // 再传缓冲区参数——转换结果直接覆盖 RAW8 数据，省下一次大内存拷贝。

    // ---- ⑦ 建立 JPEG 硬件编码器 ----
    // 这是专用硬件：编码不占 CPU，所以 P4 能在推流的同时还有余力干别的。
    jpeg_encode_engine_cfg_t jpeg_engine_config = {};
    jpeg_engine_config.timeout_ms = 200;  // 单次编码超时：超时就返回错误而不是永久卡死
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&jpeg_engine_config, &s_jpeg_encoder), TAG, "JPEG encoder init failed");

    // 分配编码输出缓冲区
    jpeg_encode_memory_alloc_cfg_t output_config = {};
    output_config.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;  // 声明「这是输出缓冲」
    // jpeg_alloc_encoder_mem 会把实际拿到的容量写回 s_jpeg_capacity
    // （可能比你申请的略大，因为要做对齐），后续按 capacity 使用才安全。
    s_jpeg_buffer = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(kJpegBufferBytes, &output_config, &s_jpeg_capacity));
    ESP_RETURN_ON_FALSE(s_jpeg_buffer != nullptr, ESP_ERR_NO_MEM, TAG, "JPEG buffer allocation failed");

    // ---- ⑧ 最后一步：让摄像头真正开始出图 ----
    // 从这一刻起，CSI 中断会持续触发，camera_task 里开始收到帧。
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(s_camera), TAG, "Camera start failed");
    return ESP_OK;
}

// =============================================================================
// 五、编码任务：把「拍好的帧」变成 JPEG
// =============================================================================
// 这是标准的「生产者-消费者」模式：
//   生产者 = CSI 中断（往队列里放缓冲区指针）
//   消费者 = 本任务（取指针 → 编码 → 更新共享 JPEG 缓冲区）
void camera_task(void *)
{
    ESP_LOGI(TAG, "Camera capture task started");

    // JPEG 编码参数：这个结构在循环外只填一次，循环里反复复用。
    jpeg_encode_cfg_t encode_config = {};
    encode_config.width = kWidth;
    encode_config.height = kHeight;
    encode_config.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;   // 输入是 ISP 转好的 RGB565
    encode_config.sub_sample = JPEG_DOWN_SAMPLING_YUV422;    // 色度采样方式，决定画质/体积
    encode_config.image_quality = 70;                        // 质量 0~100：越大越清晰也越大
    // 调优提示：quality 与 sub_sample 是「清晰度 vs 帧率」的主要旋钮。
    // 推流卡顿时，先降 quality（如 50）或降分辨率，效果最明显。

    while (true) {
        void *frame = nullptr;
        // portMAX_DELAY = 一直阻塞到有帧为止。本任务是消费者，
        // 没活干时应该让出 CPU，而不是空转浪费电和算力。
        if (xQueueReceive(s_completed_frames, &frame, portMAX_DELAY) != pdTRUE || frame == nullptr) {
            ESP_LOGW(TAG, "Camera frame queue receive failed");
            continue;
        }

        // 【极易漏掉的一步】缓存一致性同步。
        // 这块内存在 PSRAM，而 P4 有数据缓存（cache）。DMA 是绕过 cache 直接写
        // PSRAM 的，所以 CPU 的 cache 里可能还是上一帧的旧数据。DIR_M2C
        // （memory to cache）会让相关 cache 行失效，逼 CPU 重新从 PSRAM 读。
        // 漏掉这行的典型症状：画面卡住不动、或是花花绿绿的错乱条纹。
        esp_cache_msync(frame, kRawFrameBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        uint32_t encoded_size = 0;
        // 取锁才能写共享的 s_jpeg_buffer。
        // 200ms 超时的含义：如果 HTTP 任务长时间占着锁（正常不会，它只做 memcpy），
        // 这里宁可放弃这一帧也不要死等，保证采集流水线不整体卡死。
        if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            const esp_err_t encode_result = jpeg_encoder_process(s_jpeg_encoder, &encode_config,
                static_cast<const uint8_t *>(frame), kRawFrameBytes,   // 输入：RGB565 帧
                s_jpeg_buffer, s_jpeg_capacity, &encoded_size);        // 输出：JPEG 缓冲
            if (encode_result == ESP_OK) {
                // 编码成功才更新共享状态：先写数据（s_jpeg_size），再写帧号。
                // HTTP 端用「帧号变化」判断是否有新帧，顺序反了会读到半成品。
                s_jpeg_size = encoded_size;
                ++s_frame_number;
                // 日志节流：第 1 帧和每 100 帧打一次，避免刷屏拖慢串口。
                if (s_frame_number == 1 || (s_frame_number % 100) == 0) {
                    ESP_LOGI(TAG, "Camera stream active: frame=%" PRIu32 ", jpeg=%" PRIu32 " bytes",
                             s_frame_number, encoded_size);
                }
            }
            xSemaphoreGive(s_frame_mutex);   // 必须在所有分支上释放锁
            if (encode_result != ESP_OK) {
                ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(encode_result));
            }
        }
    }
}

// =============================================================================
// 六、HTTP 服务器：把 JPEG 推给手机
// =============================================================================

// 【路由 1】GET / —— 一个极简 HTML 页面，只为了让手机打开网址就能看到画面。
// 妙处在于：页面里唯一的元素是 <img src='/stream'>，
// 浏览器会自己去请求 /stream 并把返回的 MJPEG 流当作「动图」持续刷新显示。
esp_err_t index_handler(httpd_req_t *request)
{
    // static const：这个字符串放只读段，不占每次请求的栈空间。
    // HTML 特意做成了移动端自适应（viewport）且铺满宽度。
    static const char page[] =
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>ESP32-P4 Camera</title><style>body{margin:0;background:#111;color:#eee;font:16px sans-serif;text-align:center}"
        "img{display:block;width:100%;height:auto;margin:auto}</style></head>"
        "<body><img src='/stream' alt='Camera stream'></body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

// 【路由 2】GET /stream —— 核心：MJPEG 推流
//
// MJPEG over HTTP 的协议原理（这是本函数最该理解的东西）：
//   一个普通 HTTP 响应是「一个完整的文档」。而这里用
//   Content-Type: multipart/x-mixed-replace 告诉浏览器：
//   「这个响应是一个永不结束的多部分文档，每部分都是完整的一帧图片，
//     每来一部分就替换掉上一部分显示。」
//   于是浏览器不需要 JS、不需要 WebSocket，就能自动播放视频。
//
// 报文长这样（本函数负责按这个格式拼字节）：
//   HTTP/1.1 200 OK
//   Content-Type: multipart/x-mixed-replace; boundary=frame
//
//   --frame
//   Content-Type: image/jpeg
//   Content-Length: 23456
//   <JPEG 二进制数据>
//   --frame
//   Content-Type: image/jpeg
//   ...
esp_err_t stream_handler(httpd_req_t *request)
{
    // 声明本响应是「分块的多部分流」，boundary=frame 与 kBoundary 必须一致
    httpd_resp_set_type(request, "multipart/x-mixed-replace; boundary=frame");
    // 允许任意网页（含本机文件）引用这个流，方便自己在电脑上做播放器调试
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");

    // 【关键设计】为本次连接单独申请一块中转缓冲区。
    // 为什么要多这一块？因为编码任务在不停地覆盖 s_jpeg_buffer。
    // 如果直接一边持锁一边发网络数据，就得等慢速的网络发完才释放锁，
    // 采集和编码会被网络速度拖死。
    // 正确做法：持锁时只做一次快速 memcpy 把当前帧「盗」出来，立刻释放锁，
    // 然后不持锁地慢慢发网络——编码任务完全不受影响。
    // 代价：每多一个客户端就多占 1MB PSRAM。
    uint8_t *frame_copy = static_cast<uint8_t *>(heap_caps_malloc(kJpegBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (frame_copy == nullptr) {
        return ESP_ERR_NO_MEM;   // PSRAM 不够就早点失败，别硬撑
    }

    uint32_t last_frame = UINT32_MAX;  // 上一帧已发送的帧号；初值取不可能的值
    esp_err_t result = ESP_OK;
    while (true) {   // 一直循环，直到客户端断开（发送报错）才退出
        size_t jpeg_size = 0;
        uint32_t frame_number = last_frame;   // 默认与上帧相同 = 「没有新帧」
        if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            frame_number = s_frame_number;
            jpeg_size = s_jpeg_size;
            // 三重校验后才拷贝：
            //   jpeg_size > 0            —— 表示至少编码成功过一帧
            //   jpeg_size <= 容量        —— 防止异常值造成越界读
            //   frame_number != last_frame —— 去重，没有新帧就别浪费内存带宽
            if (jpeg_size > 0 && jpeg_size <= kJpegBufferBytes && frame_number != last_frame) {
                memcpy(frame_copy, s_jpeg_buffer, jpeg_size);
            }
            xSemaphoreGive(s_frame_mutex);   // 拷贝完立刻放锁，网络发送不持锁
        }

        // 判断是否拿到了「新的一帧」：条件与上面完全一致（拷贝成功才成立）
        if (jpeg_size > 0 && jpeg_size <= kJpegBufferBytes && frame_number != last_frame) {
            // 拼一个 MJPEG 分段的头部：--frame + 两个空行前的 Content-Type/Length
            char part_header[96];
            const int header_length = snprintf(part_header, sizeof(part_header),
                "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", kBoundary,
                static_cast<unsigned>(jpeg_size));
            // 分三段发送：头部 → JPEG 裸数据 → 结尾的 CRLF。
            // 用 httpd_resp_send_chunk（分块传输）而不是一次性 send，
            // 是因为流永不结束，压根算不出 Content-Length。
            result = httpd_resp_send_chunk(request, part_header, header_length);
            if (result == ESP_OK) {
                result = httpd_resp_send_chunk(request, reinterpret_cast<const char *>(frame_copy), jpeg_size);
            }
            if (result == ESP_OK) {
                result = httpd_resp_send_chunk(request, "\r\n", 2);
            }
            if (result != ESP_OK) {
                break;   // 通常是手机离开了页面 / 关了 Wi-Fi → socket 断开
            }
            last_frame = frame_number;   // 记下已发帧号，下一轮据此去重
        } else {
            // 暂时没有新帧：睡 30ms 再查。
            // 一定要 vTaskDelay 而不是忙等，否则这个任务会白占 CPU。
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    free(frame_copy);   // 连接结束，还回 PSRAM（漏掉这行会随连接次数累积泄漏，最终 OOM）
    httpd_resp_send_chunk(request, nullptr, 0);   // 发送 0 长度块 = 结束分块响应

    // 【本工程最重要的一个限制，值得单独记住】
    // ESP-IDF 的 HTTP 服务器只有 ONE 个 httpd 任务（源码里名字固定为 "httpd"），
    // 它用 select() 轮询所有 socket 后【依次】调用处理函数。
    // 而本函数在连接存续期间一直不返回，所以第一个观众一旦连上 /stream，
    // 这个唯一的 httpd 任务就被他独占了：此后别人再访问 http://192.168.4.1
    // 会一直转圈没有响应，直到第一个观众关闭页面/Wi-Fi。
    // （换句话说：本工程实际只能同时服务一个观看者。想支持多人同时看，
    //   需要改成异步处理 httpd_req_async_handler_begin()，或换用 RTSP/WebSocket。）
    // 相关的另一点：config.max_open_sockets 默认是 7，但其中 3 个被 HTTP 服务器
    // 内部占用（见 esp_http_server.h 的注释），所以实际可用于客户端的更少。
    return result;
}

esp_err_t start_http_server()
{
    // 先用官方默认配置，再按本场景的三处需求覆盖：
    // 默认值长这样（见 esp_http_server.h 的 HTTPD_DEFAULT_CONFIG）：
    //   栈 4096、优先级 空闲+5、max_open_sockets 7、max_uri_handlers 8。
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;        // 手机直接访问 http://192.168.4.1，无需端口号
    config.max_uri_handlers = 4;    // 只注册了 2 个路由，4 是留余量
    // stack_size 调大到 8KB：所有 handler 都跑在那唯一的 httpd 任务里，
    // 里面有 snprintf、日志、以及 esp_http_server 内部调用，默认 4KB 偏紧。
    config.stack_size = 8192;
    httpd_handle_t server = nullptr;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(result));
        return result;
    }

    // 注册路由：URI + HTTP 方法 + 处理函数
    httpd_uri_t index_uri = {};
    index_uri.uri = "/";
    index_uri.method = HTTP_GET;
    index_uri.handler = index_handler;
    result = httpd_register_uri_handler(server, &index_uri);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "HTTP index handler registration failed: %s", esp_err_to_name(result));
        httpd_stop(server);   // 注册失败就把服务器停掉，避免留下「半可用」状态
        return result;
    }

    httpd_uri_t stream_uri = {};
    stream_uri.uri = "/stream";
    stream_uri.method = HTTP_GET;
    stream_uri.handler = stream_handler;
    result = httpd_register_uri_handler(server, &stream_uri);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "HTTP stream handler registration failed: %s", esp_err_to_name(result));
        httpd_stop(server);
        return result;
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", config.server_port);
    return ESP_OK;
}

// =============================================================================
// 七、网络：让 P4 拥有 Wi-Fi（靠板载 ESP32-C6）
// =============================================================================
// 【本工程最容易被忽略、也最容易踩坑的一点】
// ESP32-P4 芯片内部【没有】Wi-Fi 射频。开发板上另有一颗 ESP32-C6，
// 它通过 SDIO 与 P4 相连。软件上要先用 ESP-Hosted 把链路建立起来，
// P4 才能「像用本地 Wi-Fi 一样」调用 esp_wifi_* API——调用会被自动转发给 C6。
esp_err_t start_wifi_ap()
{
    // ---- ① 初始化网络接口层和事件循环 ----
    // esp_netif 把「Wi-Fi 驱动」和「TCP/IP 协议栈」粘在一起：
    // 没有它，Wi-Fi 连上了也不会有 IP，HTTP 服务器更无法监听。
    esp_err_t result = esp_netif_init();
    // ESP_ERR_INVALID_STATE = 已经被初始化过了。这里容忍它，
    // 让函数具备「重复调用也不出错」的健壮性。
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Network interface init failed: %s", esp_err_to_name(result));
        return result;
    }
    result = esp_event_loop_create_default();   // Wi-Fi 状态变化会往这个事件循环里投递事件
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Default event loop init failed: %s", esp_err_to_name(result));
        return result;
    }
    // 创建「默认的 AP 模式网络接口」，并自动给它配上 192.168.4.1 这个地址
    // （这就是手机连上热点后要访问的 IP，由 ESP-IDF 的默认行为决定，不用自己写死）
    if (esp_netif_create_default_wifi_ap() == nullptr) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi AP interface");
        return ESP_ERR_NO_MEM;
    }

    // ---- ② 建立 ESP-Hosted 链路（P4 ↔ C6）----
    // ESP32-P4 has no local Wi-Fi radio. The A kit uses its onboard ESP32-C6
    // over SDIO, so initialize ESP-Hosted and establish the link first.
    // 中文补充：必须先 init 再 connect，顺序反了会失败。
    // 排查提示：如果日志停在 "ESP-Hosted C6 connection failed"，多半是
    // C6 里没有烧录对应的 hosted slave 固件，或者 SDIO 引脚/时钟配置不匹配。
    const esp_err_t hosted_init_result = static_cast<esp_err_t>(esp_hosted_init());
    if (hosted_init_result != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted initialization failed: %s", esp_err_to_name(hosted_init_result));
        return hosted_init_result;
    }
    const esp_err_t hosted_connect_result = static_cast<esp_err_t>(esp_hosted_connect_to_slave());
    if (hosted_connect_result != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted C6 connection failed: %s", esp_err_to_name(hosted_connect_result));
        return hosted_connect_result;
    }

    // ---- ③ 初始化并启动 SoftAP ----
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();   // 默认配置已适配常见场景
    const esp_err_t wifi_init_result = esp_wifi_init(&init_config);
    if (wifi_init_result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(wifi_init_result));
        return wifi_init_result;
    }

    wifi_config_t ap_config = {};
    // 注意 ap_config.ap.ssid 是定长数组（不是指针），必须 strncpy 拷贝。
    // 这里也用 sizeof(目标数组) 作为长度上限，天然避免溢出。
    strncpy(reinterpret_cast<char *>(ap_config.ap.ssid), kApSsid, sizeof(ap_config.ap.ssid));
    strncpy(reinterpret_cast<char *>(ap_config.ap.password), kApPassword, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(kApSsid);   // SSID 实际长度（不设的话可能带着 '\0' 后的残留）
    ap_config.ap.channel = 6;                  // 2.4GHz 第 6 信道，1/6/11 是最不重叠的三个
    ap_config.ap.max_connection = 4;           // 最多 4 个设备同时连（手机+电脑足够）
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK; // WPA2 加密；密码不足 8 位必须改成 OPEN

    // ESP_ERROR_CHECK：这里用「失败即 panic 并打印调用栈」的强校验方式。
    // 与前面的 ESP_RETURN_ON_ERROR 相比更粗暴，但也更难被忽略——
    // 对「硬件配置类、错了就没意义」的调用很合适。
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));       // 只做热点，不做 STA
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi AP started: SSID=%s, channel=%u", kApSsid, ap_config.ap.channel);
    return ESP_OK;
}

} // namespace

// =============================================================================
// 八、程序入口：app_main
// =============================================================================
// 用 extern "C" 是因为 ESP-IDF 的启动代码用 C 编译，按 C 的名字修饰去查找
// app_main 这个符号；不加会链接失败。
extern "C" void app_main(void)
{
    // ---- ① NVS 初始化 ----
    // Wi-Fi 驱动需要把校准数据、配置存进 NVS（flash 里的一块键值存储区）。
    // 两种「不算错误」的特殊返回值需要先擦除再重试：
    //   ESP_ERR_NVS_NO_FREE_PAGES    —— 分区满了
    //   ESP_ERR_NVS_NEW_VERSION_FOUND —— 数据格式版本变了（如换了 IDF 版本）
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }

    // ---- ② 初始化摄像头链路（见 initialize_camera）----
    const esp_err_t camera_result = initialize_camera();
    if (camera_result != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed: %s", esp_err_to_name(camera_result));
        return;   // 摄像头起不来，这个项目就没有意义，直接停在这里并留下日志
    }

    // ---- ③ 创建互斥锁并启动编码任务 ----
    s_frame_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_frame_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    // 参数含义：任务函数、任务名、栈大小(字节)、参数、优先级、句柄
    //   · 栈 8192：编码调用、snprintf、日志都在这条栈上，给足更稳
    //   · 优先级 5：高于空闲任务，与 httpd 任务同量级，保证编码不被饿死
    //   · 句柄传 nullptr：本程序不需要在别处操作这个任务
    const BaseType_t camera_task_result = xTaskCreate(camera_task, "camera_capture", 8192, nullptr, 5, nullptr);
    ESP_ERROR_CHECK(camera_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    // ---- ④ 起网络，再起 HTTP 服务 ----
    // 顺序很重要：HTTP 服务器必须绑定在一个已经存在的网络接口上。
    // 先起 WiFi 后起 HTTP，是唯一正确的顺序。
    const esp_err_t wifi_result = start_wifi_ap();
    if (wifi_result != ESP_OK) {
        ESP_LOGE(TAG, "Application stopped because Wi-Fi startup failed: %s", esp_err_to_name(wifi_result));
        return;
    }
    const esp_err_t http_result = start_http_server();
    if (http_result != ESP_OK) {
        ESP_LOGE(TAG, "Application stopped because HTTP startup failed: %s", esp_err_to_name(http_result));
        return;
    }

    // ---- ⑤ 打印使用说明，然后本函数返回 ----
    // app_main 返回后会被删除，但上面创建的相机任务和 HTTP 任务仍在后台运行，
    // 整个程序靠它们持续工作。这就是「入口函数结束 ≠ 程序结束」的嵌入式常见形态。
    ESP_LOGI(TAG, "Connect phone to Wi-Fi %s (password: %s), then open http://192.168.4.1", kApSsid, kApPassword);
}
