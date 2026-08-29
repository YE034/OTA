# For more information on build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
cmake_minimum_required(VERSION 3.5)

# 固件版本号（OTA 升级时平台据此判断是否需要升级）
set(PROJECT_VER "1.0.7")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sample_project)
