# Copyright (c) 2024, CogniPilot Foundation
# SPDX-License-Identifier: Apache-2.0

if (CONFIG_ZROS)

zephyr_library_named(zros)

set(ZROS_DIR ${ZEPHYR_CURRENT_MODULE_DIR})

zephyr_library_include_directories(${ZROS_DIR}/include)

zephyr_include_directories(${ZROS_DIR}/include)

zephyr_library_sources(
  ${ZROS_DIR}/src/zros_node.c
  ${ZROS_DIR}/src/zros_broker.c
  ${ZROS_DIR}/src/zros_pub.c
  ${ZROS_DIR}/src/zros_sub.c
  ${ZROS_DIR}/src/zros_topic.c
  )

endif()
