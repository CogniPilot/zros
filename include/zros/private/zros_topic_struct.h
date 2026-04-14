/*
 * Copyright (c) 2023 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZROS_TOPIC_STRUCT_H
#define ZROS_TOPIC_STRUCT_H

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/********************************************************************
 * zros topic
 ********************************************************************/
enum zros_topic_backend {
    ZROS_TOPIC_BACKEND_MUTEX = 0,
    ZROS_TOPIC_BACKEND_SINGLE_PUBLISHER = 1,
};

struct zros_topic {
    bool _initialized;
    enum zros_topic_backend _backend;
    const char* _name;
    sys_snode_t _broker_list_node;
    void* _data; // data pointer for subscriber pull
    void* _data_back; // alternate payload slot for single-publisher double buffering
    int _size; // size of data
    sys_slist_t _subs; // list of subscriptions
    sys_slist_t _pubs; // list of publications
    atomic_t _pub_count; // registered publisher count
    atomic_t _lockless_generation; // published slot generation for double buffering
    atomic_t _lockless_read_retries; // reader retries caused by concurrent publishes
    atomic_t _lockless_write_retries; // writer-side contentions for lockless publishes
    atomic_t _single_publisher_writer_claim; // atomic writer claim for single-publisher backend
    atomic_t _sub_count; // registered subscriber count for O(1) wakeup decisions
    atomic_t _legacy_poll_sub_count; // subscribers that still use deprecated poll events
    struct k_event _data_event; // topic-level update event for single-publisher wakeups
    struct k_sem _sem_read; // read semaphore
    struct k_mutex _lock_write; // write mutex
    struct k_mutex _lock_meta; // metadata lock for pub/sub lists
};


// vi: ts=4 sw=4 et
#endif // ZROS_TOPIC_STRUCT_H
