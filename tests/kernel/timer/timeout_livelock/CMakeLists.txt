# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(timeout_livelock)
target_sources(app PRIVATE src/main.c)
if(TL_SECS)
  target_compile_definitions(app PRIVATE TL_SECS=${TL_SECS})
endif()
