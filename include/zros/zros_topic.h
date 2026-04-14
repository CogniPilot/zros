/*
 * Copyright (c) 2023 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZROS_TOPIC_H
#define ZROS_TOPIC_H
#include <zephyr/kernel.h>
#include <zros/private/zros_topic_struct.h>

/********************************************************************
 * zros topic
 *
 * ZROS_TOPIC_DEFINE():
 *   Compatibility default. Uses the existing mutex/semaphore backend.
 *
 * ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER():
 *   RDD2-oriented latest-value backend for topics with one registered
 *   publisher. Uses double-buffered payload storage, atomic writer
 *   arbitration, subscriber-side generation checks, and topic-level event
 *   wakeups with O(1) publish-side notification.
 ********************************************************************/

#define ZROS_TOPIC_DEFINE(NAME, TYPE)                                  \
    static TYPE g_msg_##NAME = {};                                     \
    struct zros_topic topic_##NAME = {                                 \
        ._backend = ZROS_TOPIC_BACKEND_MUTEX,                          \
        ._name = #NAME,                                                \
        ._data = &g_msg_##NAME,                                        \
        ._data_back = NULL,                                            \
        ._size = sizeof(g_msg_##NAME),                                 \
        ._subs = SYS_SLIST_STATIC_INIT(topic_##NAME._subs),            \
        ._pubs = SYS_SLIST_STATIC_INIT(topic_##NAME._pubs),            \
        ._pub_count = ATOMIC_INIT(0),                                  \
        ._broker_list_node = {                                         \
            .next = NULL,                                              \
        },                                                             \
        ._lockless_generation = ATOMIC_INIT(0),                        \
        ._lockless_read_retries = ATOMIC_INIT(0),                      \
        ._lockless_write_retries = ATOMIC_INIT(0),                     \
        ._single_publisher_writer_claim = ATOMIC_INIT(0),              \
        ._sub_count = ATOMIC_INIT(0),                                  \
        ._legacy_poll_sub_count = ATOMIC_INIT(0),                      \
        ._data_event = Z_EVENT_INITIALIZER(topic_##NAME._data_event),  \
        ._sem_read = Z_SEM_INITIALIZER(topic_##NAME._sem_read, 6, 6),  \
        ._lock_write = Z_MUTEX_INITIALIZER(topic_##NAME._lock_write),  \
        ._lock_meta = Z_MUTEX_INITIALIZER(topic_##NAME._lock_meta),    \
    };

#define ZROS_TOPIC_DEFINE_MUTEX(NAME, TYPE) \
    ZROS_TOPIC_DEFINE(NAME, TYPE)

#define ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(NAME, TYPE)                 \
    static TYPE g_msg_##NAME[2] = {};                                  \
    struct zros_topic topic_##NAME = {                                 \
        ._backend = ZROS_TOPIC_BACKEND_SINGLE_PUBLISHER,               \
        ._name = #NAME,                                                \
        ._data = &g_msg_##NAME[0],                                     \
        ._data_back = &g_msg_##NAME[1],                                \
        ._size = sizeof(g_msg_##NAME[0]),                              \
        ._subs = SYS_SLIST_STATIC_INIT(topic_##NAME._subs),            \
        ._pubs = SYS_SLIST_STATIC_INIT(topic_##NAME._pubs),            \
        ._pub_count = ATOMIC_INIT(0),                                  \
        ._broker_list_node = {                                         \
            .next = NULL,                                              \
        },                                                             \
        ._lockless_generation = ATOMIC_INIT(0),                        \
        ._lockless_read_retries = ATOMIC_INIT(0),                      \
        ._lockless_write_retries = ATOMIC_INIT(0),                     \
        ._single_publisher_writer_claim = ATOMIC_INIT(0),              \
        ._sub_count = ATOMIC_INIT(0),                                  \
        ._legacy_poll_sub_count = ATOMIC_INIT(0),                      \
        ._data_event = Z_EVENT_INITIALIZER(topic_##NAME._data_event),  \
        ._sem_read = Z_SEM_INITIALIZER(topic_##NAME._sem_read, 6, 6),  \
        ._lock_write = Z_MUTEX_INITIALIZER(topic_##NAME._lock_write),  \
        ._lock_meta = Z_MUTEX_INITIALIZER(topic_##NAME._lock_meta),    \
    };

#define ZROS_TOPIC_DECLARE(NAME, TYPE) \
    extern struct zros_topic topic_##NAME;

// forward declarations
struct zros_topic;
struct zros_sub;
struct zros_pub;

// public api
typedef void zros_pub_iterator_t(const struct zros_pub* pub, void* data);
typedef void zros_sub_iterator_t(const struct zros_sub* sub, void* data);
int zros_topic_publish(struct zros_topic* topic, void* data);
int zros_topic_read(struct zros_topic* topic, void* data);
int zros_topic_get_name(const struct zros_topic* node, char* buf, size_t n);
int zros_topic_add_pub(struct zros_topic* topic, struct zros_pub* pub);
int zros_topic_remove_pub(struct zros_topic* topic, struct zros_pub* pub);
int zros_topic_add_sub(struct zros_topic* topic, struct zros_sub* sub);
int zros_topic_remove_sub(struct zros_topic* topic, struct zros_sub* sub);
int zros_topic_iterate_pub(struct zros_topic* topic, zros_pub_iterator_t* iter, void* data);
int zros_topic_iterate_sub(struct zros_topic* topic, zros_sub_iterator_t* iter, void* data);

// vi: ts=4 sw=4 et
#endif // ZROS_TOPIC_H
