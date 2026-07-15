/*
 * Copyright (c) 2023 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/private/zros_topic_struct.h>
#include <zros/private/zros_wait_internal.h>
#include <zros/zros_common.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>
#include <zros/zros_topic.h>

/********************************************************************
 * zros topic
 ********************************************************************/

LOG_MODULE_DECLARE(zros);
K_EVENT_DEFINE(g_zros_wait_any_event);

static const k_timeout_t g_topic_timeout = K_MSEC(1);
#define ZROS_TOPIC_EVENT_UPDATED BIT(0)

static bool _zros_topic_is_single_publisher(const struct zros_topic* topic)
{
    return topic->_backend == ZROS_TOPIC_BACKEND_SINGLE_PUBLISHER;
}

static bool _zros_topic_uses_double_buffer(const struct zros_topic* topic)
{
    return _zros_topic_is_single_publisher(topic);
}

static inline void _zros_topic_test_yield_loop(int count)
{
    for (int i = 0; i < count; ++i) {
        k_yield();
    }
}

static inline void* _zros_topic_double_buffer_slot(struct zros_topic* topic, uint32_t generation)
{
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(topic->_data != NULL, "zros topic data is null");
    __ASSERT(topic->_data_back != NULL, "zros topic double-buffer back slot is null");
    return ((generation & 1U) == 0U) ? topic->_data : topic->_data_back;
}

static int64_t _zros_topic_sub_min_interval_ticks(const struct zros_sub* sub)
{
    double interval_ticks;
    int64_t rounded_ticks;

    if (sub->_rate_limit_hz <= 0.0) {
        return 0;
    }

    interval_ticks = (double)CONFIG_SYS_CLOCK_TICKS_PER_SEC / sub->_rate_limit_hz;
    if (interval_ticks <= 1.0) {
        return 1;
    }

    rounded_ticks = (int64_t)interval_ticks;
    if ((double)rounded_ticks < interval_ticks) {
        rounded_ticks++;
    }
    return rounded_ticks;
}

static bool _zros_topic_sub_rate_limit_allows(const struct zros_sub* sub, int64_t now_ticks)
{
    int64_t min_interval_ticks;

    if (sub->_last_update_ticks == 0) {
        return true;
    }

    min_interval_ticks = _zros_topic_sub_min_interval_ticks(sub);
    if (min_interval_ticks <= 0) {
        return true;
    }

    return (now_ticks - sub->_last_update_ticks) >= min_interval_ticks;
}

static int _zros_topic_meta_lock(struct zros_topic* topic)
{
    __ASSERT(topic != NULL, "zros topic is null");
    return k_mutex_lock(&topic->_lock_meta, g_topic_timeout);
}

static void _zros_topic_meta_unlock(struct zros_topic* topic)
{
    __ASSERT(topic != NULL, "zros topic is null");
    k_mutex_unlock(&topic->_lock_meta);
}

int _zros_topic_write_lock(struct zros_topic* topic)
{
    __ASSERT(topic != NULL, "zros topic is null");
    struct k_sem* read = &topic->_sem_read;
    struct k_mutex* write = &topic->_lock_write;

    // take write semaphore
    ZROS_RC(k_mutex_lock(write, g_topic_timeout),
        LOG_ERR("write lock failed\n");
        return rc);

    // take read semaphore
    unsigned int read_take_count = 0;
    while (true) {
        int rc = k_sem_take(read, g_topic_timeout);
        if (rc != 0) {
            char name[20];
            zros_topic_get_name(topic, name, sizeof(name));
            LOG_ERR("topic %s take read: %u/%u failed\n", name, read_take_count, read->limit);
            for (size_t i = 0; i < read_take_count; i++) {
                k_sem_give(read);
            }
            k_mutex_unlock(write);
            return rc;
        }
        if (++read_take_count >= read->limit)
            break;
    }
    return ZROS_OK;
}

void _zros_topic_write_unlock(struct zros_topic* topic)
{
    __ASSERT(topic != NULL, "zros topic is null");
    struct k_sem* read = &topic->_sem_read;
    struct k_mutex* write = &topic->_lock_write;

    // release read locks
    unsigned int read_take_count = read->limit;
    while (true) {
        k_sem_give(read);
        if (--read_take_count == 0)
            break;
    }

    // release write
    k_mutex_unlock(write);
};

int _zros_topic_read_lock(const struct zros_topic* topic)
{
    __ASSERT(topic != NULL, "zros topic is null");
    struct k_sem* read = (struct k_sem*)&topic->_sem_read;
    int ret = k_sem_take(read, g_topic_timeout);
    if (ret < 0) {
        char name[20];
        zros_topic_get_name(topic, name, sizeof(name));
        LOG_WRN("topic %s take read failed\n", name);
        return ret;
    }
    return ZROS_OK;
};

void _zros_topic_read_unlock(const struct zros_topic* topic)
{
    __ASSERT(topic != NULL, "zros topic is null");
    struct k_sem* read = (struct k_sem*)&topic->_sem_read;
    k_sem_give(read);
};

static int _zros_topic_double_buffer_read(struct zros_topic* topic, void* data,
    uint32_t* generation_out)
{
    uint32_t generation_before;
    uint32_t generation_after;
    const void* source;

    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(data != NULL, "zros data is null");
    __ASSERT(topic->_data_back != NULL, "zros topic double-buffer back slot is null");

    while (true) {
        generation_before = (uint32_t)atomic_get(&topic->_lockless_generation);
        source = _zros_topic_double_buffer_slot(topic, generation_before);

        if (CONFIG_ZROS_LOCKLESS_TEST_READER_PROBE_YIELDS > 0) {
            _zros_topic_test_yield_loop(CONFIG_ZROS_LOCKLESS_TEST_READER_PROBE_YIELDS);
        }

        memcpy(data, source, topic->_size);
        generation_after = (uint32_t)atomic_get(&topic->_lockless_generation);

        if (generation_before == generation_after) {
            if (generation_out != NULL) {
                *generation_out = generation_after;
            }
            return ZROS_OK;
        }

        atomic_inc(&topic->_lockless_read_retries);
    }
}

static int _zros_topic_single_publisher_write(struct zros_topic* topic, const void* data)
{
    uint32_t generation;
    uint32_t next_generation;
    void* target;

    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(data != NULL, "zros data is null");
    __ASSERT(topic->_data_back != NULL, "zros topic double-buffer back slot is null");

    while (true) {
        if (atomic_cas(&topic->_single_publisher_writer_claim, 0, 1)) {
            break;
        }
        atomic_inc(&topic->_lockless_write_retries);
        k_yield();
    }

    generation = (uint32_t)atomic_get(&topic->_lockless_generation);
    next_generation = generation + 1U;
    target = _zros_topic_double_buffer_slot(topic, next_generation);
    memcpy(target, data, topic->_size);

    if (CONFIG_ZROS_LOCKLESS_TEST_WRITER_HOLD_YIELDS > 0) {
        _zros_topic_test_yield_loop(CONFIG_ZROS_LOCKLESS_TEST_WRITER_HOLD_YIELDS);
    }

    atomic_set(&topic->_lockless_generation, (atomic_val_t)next_generation);
    atomic_set(&topic->_single_publisher_writer_claim, 0);

    if (atomic_get(&topic->_sub_count) > 0) {
        k_event_post(&topic->_data_event, ZROS_TOPIC_EVENT_UPDATED);
    }

    return ZROS_OK;
}

static void _zros_topic_notify_subscribers_locked(struct zros_topic* topic, int64_t now)
{
    struct zros_sub* sub;

    __ASSERT(topic != NULL, "zros topic is null");

    SYS_SLIST_FOR_EACH_CONTAINER(
        &topic->_subs, sub, _topic_list_node)
    {
        if (_zros_topic_sub_rate_limit_allows(sub, now)) {
            k_poll_signal_raise(&sub->_data_ready, 1);
            sub->_last_update_ticks = now;
        }
    }
}

static void _zros_topic_notify_legacy_poll_subscribers(struct zros_topic* topic)
{
    struct zros_sub* sub;

    __ASSERT(topic != NULL, "zros topic is null");

    if (atomic_get(&topic->_legacy_poll_sub_count) == 0) {
        return;
    }

    ZROS_RC(_zros_topic_meta_lock(topic),
        LOG_ERR("topic metadata lock failed");
        return);

    SYS_SLIST_FOR_EACH_CONTAINER(
        &topic->_subs, sub, _topic_list_node)
    {
        if (sub->_legacy_poll_enabled) {
            k_poll_signal_raise(&sub->_data_ready, 1);
        }
    }

    _zros_topic_meta_unlock(topic);
}

int zros_topic_add_pub(struct zros_topic* topic, struct zros_pub* pub)
{
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(pub != NULL, "zros pub is null");
    ZROS_RC(_zros_topic_meta_lock(topic),
        LOG_ERR("pub metadata lock failed");
        return rc);

    if (_zros_topic_is_single_publisher(topic) && !sys_slist_is_empty(&topic->_pubs)) {
        _zros_topic_meta_unlock(topic);
        return -EALREADY;
    }

    sys_slist_append(&topic->_pubs, &pub->_topic_list_node);
    _zros_topic_meta_unlock(topic);
    return ZROS_OK;
}

int zros_topic_remove_pub(struct zros_topic* topic, struct zros_pub* pub)
{
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(pub != NULL, "zros pub is null");
    ZROS_RC(_zros_topic_meta_lock(topic),
        LOG_ERR("pub metadata lock failed");
        return rc);
    sys_slist_find_and_remove(&topic->_pubs, &pub->_topic_list_node);
    _zros_topic_meta_unlock(topic);
    return ZROS_OK;
}

int zros_topic_add_sub(struct zros_topic* topic, struct zros_sub* sub)
{
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(sub != NULL, "zros sub is null");
    ZROS_RC(_zros_topic_meta_lock(topic),
        LOG_WRN("topic metadata lock failed");
        return rc);
    sys_slist_append(&topic->_subs, &sub->_topic_list_node);
    atomic_inc(&topic->_sub_count);
    _zros_topic_meta_unlock(topic);
    return ZROS_OK;
}

int zros_topic_remove_sub(struct zros_topic* topic, struct zros_sub* sub)
{
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(sub != NULL, "zros sub is null");
    ZROS_RC(_zros_topic_meta_lock(topic),
        LOG_ERR("topic metadata lock failed");
        return rc);
    if (sys_slist_find_and_remove(&topic->_subs, &sub->_topic_list_node)) {
        atomic_dec(&topic->_sub_count);
    }
    _zros_topic_meta_unlock(topic);
    return ZROS_OK;
}

int zros_topic_publish(struct zros_topic* topic, void* data)
{
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(data != NULL, "zros data is null");
    int64_t now = 0;
    int rc;

    if (_zros_topic_is_single_publisher(topic)) {
        rc = _zros_topic_single_publisher_write(topic, data);
    } else {
        rc = _zros_topic_write_lock(topic);
        if (rc < 0) {
            LOG_ERR("topic r/w lock failed");
            return rc;
        }

        rc = _zros_topic_meta_lock(topic);
        if (rc < 0) {
            _zros_topic_write_unlock(topic);
            LOG_ERR("topic metadata lock failed");
            return rc;
        }

        memcpy(topic->_data, data, topic->_size);
        now = k_uptime_ticks();
        _zros_topic_notify_subscribers_locked(topic, now);
        _zros_topic_meta_unlock(topic);
        _zros_topic_write_unlock(topic);
    }

    if (rc < 0) {
        return rc;
    }

    if (_zros_topic_is_single_publisher(topic)) {
        _zros_topic_notify_legacy_poll_subscribers(topic);
        k_event_post(&g_zros_wait_any_event, ZROS_WAIT_ANY_EVENT_UPDATED);
        return ZROS_OK;
    }

    k_event_post(&g_zros_wait_any_event, ZROS_WAIT_ANY_EVENT_UPDATED);
    return ZROS_OK;
}

int zros_topic_read(struct zros_topic* topic, void* data)
{
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(data != NULL, "zros data is null");

    if (_zros_topic_uses_double_buffer(topic)) {
        return _zros_topic_double_buffer_read(topic, data, NULL);
    }

    int rc = _zros_topic_read_lock(topic);
    if (rc < 0) {
        LOG_ERR("topic read lock failed");
        return rc;
    }
    memcpy(data, topic->_data, topic->_size);
    _zros_topic_read_unlock(topic);
    return ZROS_OK;
}

int zros_topic_get_name(const struct zros_topic* topic, char* buf, size_t n)
{
    __ASSERT(topic != NULL, "zros topic is null");
    // name is const char, threadsafe
    ZROS_RC(snprintf(buf, n, "%s", topic->_name), return rc);
    return ZROS_OK;
};

int zros_topic_iterate_pub(struct zros_topic* topic, zros_pub_iterator_t* iter, void* data)
{
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(iter != NULL, "zros pub iterator is null");
    ZROS_RC(_zros_topic_meta_lock(topic),
        LOG_ERR("topic metadata lock failed");
        return rc);
    struct zros_pub* pub;
    SYS_SLIST_FOR_EACH_CONTAINER(
        &topic->_pubs, pub, _topic_list_node)
    {
        __ASSERT(pub != NULL, "pub is null");
        iter(pub, data);
    }
    _zros_topic_meta_unlock(topic);
    return ZROS_OK;
}

int zros_topic_iterate_sub(struct zros_topic* topic, zros_sub_iterator_t* iter, void* data)
{
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(iter != NULL, "zros sub iterator is null");
    ZROS_RC(_zros_topic_meta_lock(topic),
        LOG_ERR("topic metadata lock failed");
        return rc);
    struct zros_sub* sub;
    SYS_SLIST_FOR_EACH_CONTAINER(
        &topic->_subs, sub, _topic_list_node)
    {
        __ASSERT(sub != NULL, "sub is null");
        iter(sub, data);
    }
    _zros_topic_meta_unlock(topic);
    return ZROS_OK;
}

// vi: ts=4 sw=4 et
