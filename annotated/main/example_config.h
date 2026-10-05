// =============================================================================
//  example_config.h —— 板级接线与摄像头格式配置（带注释副本）
// =============================================================================
//  这个头文件是「硬件相关参数」的唯一出处：
//  当你的摄像头模块型号、接线、分辨率与开发板不同时，改这里（以及 sdkconfig.defaults），
//  而不用去动 main.cpp 里的逻辑代码。这就是把配置与代码分离的价值。
#pragma once
// ↑ 头文件卫士的现代写法：保证同一编译单元里被 include 多次也只展开一次。
//   等价于传统的 #ifndef XXX_H / #define XXX_H / #endif 三件套，但不会重名。

// MIPI-CSI wiring used by the Waveshare ESP32-P4 Module DEV KIT camera port.
// 中文补充：这三个 IO 号是 Waveshare ESP32-P4 Module DEV KIT 摄像头接口的固定接线，
// 查开发板原理图即可确认。换成别的板子必须改，否则传感器探测会失败。
//
// SCCB = Serial Camera Control Bus，是 OV 系列摄像头的事实标准控制总线，
// 电气特性与 I2C 兼容，所以这里直接复用 P4 的 I2C 主机来驱动它。

// SCCB / I2C 时钟线，接 GPIO8
#define EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO 8
// SCCB / I2C 数据线，接 GPIO7
#define EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO 7

// 每条 MIPI 数据 lane 的速率：200 Mbps。
// 这个值必须落在「传感器输出能力」与「P4 CSI 接收能力」的交集里，
// 通常直接照抄官方例程或模组手册给出的一组可用参数。
// 数值不匹配的典型症状：收不到帧、或图像出现规律性条纹。
#define EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS 200

// 要使用的传感器输出格式名：2 lane / 24MHz 输入时钟 / RAW8 / 800x640 / 50fps。
// 【重要】这不是随便起的名字，而是传感器驱动内部注册表里的 key
// （本工程用的是 OV5647，可在
//   managed_components/espressif__esp_cam_sensor/sensors/ov5647/ov5647.c
//  里搜 "MIPI_2lane_24Minput_RAW8_800x640_50fps" 看到它的定义）。
// 名字对不上 → 传感器初始化直接失败。
// 同时它把「分辨率」这件事也定了下来：必须与 main.cpp 里的 kWidth/kHeight 保持一致。
// 想换分辨率就三处一起改：本行的格式名、main.cpp 的 kWidth/kHeight、
// 以及 sdkconfig.defaults 里的 CONFIG_CAMERA_OV5647_MIPI_RAW8_... 选项。
#define EXAMPLE_CAM_FORMAT "MIPI_2lane_24Minput_RAW8_800x640_50fps"
