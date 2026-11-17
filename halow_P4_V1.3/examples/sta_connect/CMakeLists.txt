# Copyright 2026 Morse Micro
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32p4 CACHE STRING "Target for the Elecrow P4 HaLow board")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sta_connect)
