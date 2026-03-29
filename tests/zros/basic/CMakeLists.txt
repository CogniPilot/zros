# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
get_filename_component(ZROS_MODULE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
list(APPEND EXTRA_ZEPHYR_MODULES ${ZROS_MODULE_ROOT})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zros_basic)

target_sources(app PRIVATE src/main.c)
