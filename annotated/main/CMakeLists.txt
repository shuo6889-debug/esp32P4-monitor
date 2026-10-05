# =============================================================================
#  main/CMakeLists.txt —— main 组件的构建描述（带注释副本）
# =============================================================================
#  ESP-IDF 的构建系统里，每个组件（component）由自己的 CMakeLists.txt 描述。
#  main 是一个特殊组件：它总是会被构建，并且它的 app_main() 是程序入口。
#
#  idf_component_register 的一次调用就完成了三件事：
#    1. 告诉构建系统「这个组件有哪些源文件」
#    2. 声明「本组件对外暴露哪些头文件目录」
#    3. 声明「本组件依赖哪些别的组件」
#
#  第 3 点最容易被初学者忽略：ESP-IDF 里不写 REQUIRES 就找不到头文件，
#  即使那个组件其实已经存在于工程里。这是和「直接 #include 系统头文件」
#  完全不同的心智模型，务必记住。
# =============================================================================

idf_component_register(SRCS "main.cpp"      # 源文件列表：本工程逻辑全在这一个文件里
                    INCLUDE_DIRS "."        # 对外暴露的头文件目录：当前目录（里面有 example_config.h）
                    REQUIRES esp_driver_cam esp_driver_isp esp_driver_i2c
                    # ↑ 摄像头控制器 / ISP / I2C 三个驱动组件
                             esp_driver_jpeg esp_http_server esp_wifi esp_event
                    # ↑ JPEG 硬件编码器 / HTTP 服务器 / Wi-Fi / 事件循环
                             esp_netif esp_hw_support esp_mm nvs_flash sensor_init)
                    # ↑ 网络接口层 / 底层硬件支持(esp_ldo_*) / 内存与cache(esp_cache_msync)
                    #   / NVS 存储 / 官方例程提供的传感器自动探测组件
                    #
                    # 对照 main.cpp 的 include 列表就能看懂：
                    # 写代码时 include 一个头文件，就必须在这里加对应的组件名，
                    # 否则编译报 "fatal error: xxx.h: No such file or directory"。
                    #
                    # 补充：有些组件（如 freertos/*、esp_log.h、driver/i2c_master.h 所属组件）
                    # 是 IDF 的「公共依赖」，会被自动加入，所以不必写在这里。
