/*
 * Copyright (c) 2023 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZROS_SUB_H
#define ZROS_SUB_H

#include <zephyr/kernel.h>

/********************************************************************
 * zros_sub
 ********************************************************************/
// forward declarations
struct zros_sub;
struct zros_topic;
struct zros_node;

// public api
struct zros_node;
int zros_sub_init(struct zros_sub* sub, struct zros_node* node, struct zros_topic* topic, void* data,
    double rate_limit_hz);
// zros_sub_update() is the only consumer of a pending update: it copies the
// topic data and clears the pending state, or returns -EAGAIN when nothing is
// pending. zros_sub_update_available() only reports whether the next
// zros_sub_update() would succeed, so repeated calls return the same answer
// until zros_sub_update() runs.
int zros_sub_update(struct zros_sub* sub);
bool zros_sub_update_available(struct zros_sub* sub);
int zros_sub_wait(struct zros_sub* sub, k_timeout_t timeout);
int zros_sub_wait_many(struct zros_sub* const* subs, size_t count, k_timeout_t timeout);
void zros_sub_fini(struct zros_sub* sub);
__deprecated struct k_poll_event* zros_sub_get_event(struct zros_sub* sub);
void zros_sub_get_node(struct zros_sub* sub, struct zros_node** node);

#endif // ZROS_SUB_H
// vi: ts=4 sw=4 et
