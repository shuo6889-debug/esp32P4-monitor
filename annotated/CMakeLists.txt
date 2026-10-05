# =============================================================================
#  根 CMakeLists.txt —— 工程的构建入口（带注释副本）
# =============================================================================
#  这是 CMake 进入这个工程时读到的第一个文件。
#  它本身几乎没有内容，真正的构建逻辑全在 ESP-IDF 提供的 project.cmake 里。
# =============================================================================

# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
# 中文补充：中文意思是「以下样板必须严格按这个顺序写」。
# 顺序错（比如把 project() 写在 include 前面）会导致 IDF 的构建变量没准备好，
# 报出一堆莫名其妙的错。ESP-IDF 工程的这个文件基本不用改，照抄即可。
cmake_minimum_required(VERSION 3.22)
# ↑ 声明需要的 CMake 最低版本。写低了新版特性用不了，写高了老环境直接报错。

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# ↑ 引入 ESP-IDF 的构建系统。$ENV{IDF_PATH} 是运行 idf.py 时环境里设置的
#   IDF 安装路径（本机为 D:\ESP_IDF\v6.1\esp-idf）。
#   这一行是「普通 CMake 工程」变成「ESP-IDF 工程」的开关。

project(jianshi)
# ↑ 声明工程名（也是最终生成的固件 .bin 文件名的前缀）。
#   它必须在 include(...project.cmake) 之后调用。
#
# 构建流程串起来是这样：
#   根 CMakeLists.txt → project.cmake 扫描所有组件
#   → 读各组件 CMakeLists.txt 与 idf_component.yml
#   → 由 main/ 组件产出主固件，与 bootloader、分区表一起打包成 jianshi.bin
