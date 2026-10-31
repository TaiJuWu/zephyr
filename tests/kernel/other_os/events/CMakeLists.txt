# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(other_os_events)

target_sources(app PRIVATE src/main.c src/rtthread_event.c src/nuttx_nxevent.c
			    src/riot_thread_flags.c)
