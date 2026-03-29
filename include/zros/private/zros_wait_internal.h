/*
 * Copyright (c) 2023 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZROS_WAIT_INTERNAL_H
#define ZROS_WAIT_INTERNAL_H

#include <zephyr/kernel.h>

#define ZROS_WAIT_ANY_EVENT_UPDATED BIT(0)

extern struct k_event g_zros_wait_any_event;

#endif // ZROS_WAIT_INTERNAL_H
