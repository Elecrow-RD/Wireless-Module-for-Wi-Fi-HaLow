cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32p4 CACHE STRING "Target for the Elecrow P4 camera board")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(web_camera_serve)

# esp_video 1.4.1 leaves newer IDF AWB fields uninitialized on P4 rev >= 3.0.
include(cmake/esp_video_awb_compat.cmake)
