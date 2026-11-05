# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
# ESP-IDF 6.1 要求 cmake_minimum_required >= 3.16；旧值 3.5 在 CMake 4.x 下会
# 打 "Compatibility with CMake < 3.10 will be removed" 弃用警告。取 3.16 作为下限。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wno-format")

# 应用版本（写入 esp_app_desc_t，启动日志 / idf.py size / OTA 会读取）。
# 不设置时 ESP-IDF 用 `git describe --tags`，会得到 "0.9.0.0_alpha_5-25-gb8c87c4-dirty"
# 这类字符串，与设置页「关于」显示的 CONFIG_FIRMWARE_VERSION_STRING 不一致。
set(PROJECT_VER "1.0.0.0")

project(FakePodNanoNightPlayer)
