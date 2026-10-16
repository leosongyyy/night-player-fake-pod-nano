# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
# ESP-IDF 6.1 要求 cmake_minimum_required >= 3.16；旧值 3.5 在 CMake 4.x 下会
# 打 "Compatibility with CMake < 3.10 will be removed" 弃用警告。取 3.16 作为下限。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wno-format")
project(FakePodNanoNightPlayer)
