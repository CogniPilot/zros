/*
 * Copyright (c) 2023 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>

#include <zros/private/zros_sub_struct.h>
#include <zros/private/zros_topic_struct.h>
#include <zros/private/zros_wait_internal.h>
#include <zros/zros_common.h>
#include <zros/zros_node.h>
#include <zros/zros_sub.h>
#include <zros/zros_topic.h>

/********************************************************************
 * zros_sub
 ********************************************************************/

LOG_MODULE_DECLARE(zros);

#define ZROS_TOPIC_EVENT_UPDATED BIT(0)

static bool _zros_sub_is_single_publisher(const struct zros_sub* sub)
{
    return sub->_topic->_backend == ZROS_TOPIC_BACKEND_SINGLE_PUBLISHER;
}

static bool _zros_sub_signal_is_pending(struct zros_sub* sub)
{
    unsigned int signaled = 0U;
    int result = 0;

    k_poll_signal_check(&sub->_data_ready, &signaled, &result);
    ARG_UNUSED(result);
    return signaled != 0U;
}

static void _zros_sub_signal_reset(struct zros_sub* sub)
{
    k_poll_signal_reset(&sub->_data_ready);
    sub->_event.state = K_POLL_STATE_NOT_READY;
}

static int64_t _zros_sub_min_interval_ticks(const struct zros_sub* sub)
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

static bool _zros_sub_rate_limit_allows(struct zros_sub* sub, int64_t now_ticks)
{
    int64_t min_interval_ticks;

    if (sub->_last_update_ticks == 0) {
        return true;
    }

    min_interval_ticks = _zros_sub_min_interval_ticks(sub);
    if (min_interval_ticks <= 0) {
        return true;
    }

    return (now_ticks - sub->_last_update_ticks) >= min_interval_ticks;
}

static k_timeout_t _zros_sub_rate_limit_timeout(struct zros_sub* sub, int64_t now_ticks)
{
    int64_t min_interval_ticks;
    int64_t remaining_ticks;

    min_interval_ticks = _zros_sub_min_interval_ticks(sub);
    if (min_interval_ticks <= 0 || sub->_last_update_ticks == 0) {
        return K_NO_WAIT;
    }

    remaining_ticks = (sub->_last_update_ticks + min_interval_ticks) - now_ticks;
    if (remaining_ticks <= 0) {
        return K_NO_WAIT;
    }

    return K_TICKS(remaining_ticks);
}

static inline void _zros_sub_test_yield_loop(int count)
{
    for (int i = 0; i < count; ++i) {
        k_yield();
    }
}

static inline const void* _zros_sub_double_buffer_slot(const struct zros_topic* topic,
    uint32_t generation)
{
    return ((generation & 1U) == 0U) ? topic->_data : topic->_data_back;
}

static bool _zros_sub_single_publisher_ready(struct zros_sub* sub, int64_t now_ticks)
{
    uint32_t generation = (uint32_t)atomic_get(&sub->_topic->_lockless_generation);

    return (generation != sub->_last_seen_generation) && _zros_sub_rate_limit_allows(sub, now_ticks);
}

static void _zros_sub_single_publisher_legacy_signal_consume(struct zros_sub* sub)
{
    if (sub->_legacy_poll_enabled && _zros_sub_signal_is_pending(sub)) {
        _zros_sub_signal_reset(sub);
    }
}

static bool _zros_sub_peek_update_available(struct zros_sub* sub, int64_t now_ticks)
{
    if (_zros_sub_is_single_publisher(sub)) {
        return _zros_sub_single_publisher_ready(sub, now_ticks);
    }

    return _zros_sub_signal_is_pending(sub);
}

static k_timeout_t _zros_sub_next_ready_timeout(struct zros_sub* sub, int64_t now_ticks)
{
    uint32_t generation;

    if (!_zros_sub_is_single_publisher(sub)) {
        return K_FOREVER;
    }

    generation = (uint32_t)atomic_get(&sub->_topic->_lockless_generation);
    if (generation == sub->_last_seen_generation) {
        return K_FOREVER;
    }

    return _zros_sub_rate_limit_timeout(sub, now_ticks);
}

static bool _zros_timeout_is_shorter(k_timeout_t candidate, k_timeout_t current)
{
    if (K_TIMEOUT_EQ(candidate, K_FOREVER)) {
        return false;
    }

    if (K_TIMEOUT_EQ(current, K_FOREVER)) {
        return true;
    }

    return candidate.ticks < current.ticks;
}

static int _zros_sub_single_publisher_update(struct zros_sub* sub)
{
    struct zros_topic* topic = sub->_topic;
    uint32_t generation_before;
    uint32_t generation_after;
    const void* source;

    while (true) {
        generation_before = (uint32_t)atomic_get(&topic->_lockless_generation);
        source = _zros_sub_double_buffer_slot(topic, generation_before);

        if (CONFIG_ZROS_LOCKLESS_TEST_READER_PROBE_YIELDS > 0) {
            _zros_sub_test_yield_loop(CONFIG_ZROS_LOCKLESS_TEST_READER_PROBE_YIELDS);
        }

        memcpy(sub->_data, source, topic->_size);
        generation_after = (uint32_t)atomic_get(&topic->_lockless_generation);

        if (generation_before == generation_after) {
            sub->_last_seen_generation = generation_after;
            sub->_last_update_ticks = k_uptime_ticks();
            return 0;
        }

        atomic_inc(&topic->_lockless_read_retries);
    }
}

int zros_sub_init(struct zros_sub* sub, struct zros_node* node, struct zros_topic* topic, void* data,
    double rate_limit_hz)
{
    __ASSERT(sub != NULL, "zros sub is null");
    __ASSERT(node != NULL, "zros node is null");
    __ASSERT(topic != NULL, "zros topic is null");
    __ASSERT(data != NULL, "zros data is null");

    // add pub to node
    ZROS_RC(zros_node_add_sub(node, sub),
        LOG_ERR("failed to add push sub to node");
        return rc);

    // set data
    sub->_topic = topic;
    sub->_data = data;
    k_poll_signal_init(&sub->_data_ready);
    sub->_legacy_poll_enabled = false;
    sub->_rate_limit_hz = rate_limit_hz;
    sub->_last_update_ticks = 0;
    sub->_last_seen_generation = (uint32_t)atomic_get(&topic->_lockless_generation);
    sub->_node_list_node.next = NULL;
    sub->_topic_list_node.next = NULL;
    k_poll_event_init(&sub->_event, K_POLL_TYPE_SIGNAL,
        K_POLL_MODE_NOTIFY_ONLY, &sub->_data_ready);
    sub->_node = node;
    sub->_initialized = true;
    return zros_topic_add_sub(topic, sub);
}

int zros_sub_update(struct zros_sub* sub)
{
    __ASSERT(sub != NULL, "zros sub is null");
    __ASSERT(sub->_initialized, "zros sub not initialized");

    if (_zros_sub_is_single_publisher(sub)) {
        if (!zros_sub_update_available(sub)) {
            return -EAGAIN;
        }

        return _zros_sub_single_publisher_update(sub);
    }

    // the mutex backend consumes the pending signal here, so that
    // zros_sub_update_available() stays a side effect free predicate
    if (!_zros_sub_signal_is_pending(sub)) {
        return -EAGAIN;
    }

    _zros_sub_signal_reset(sub);
    return zros_topic_read(sub->_topic, sub->_data);
}

bool zros_sub_update_available(struct zros_sub* sub)
{
    int64_t now_ticks;

    __ASSERT(sub != NULL, "zros sub is null");
    __ASSERT(sub->_initialized, "zros sub not initialized");

    now_ticks = k_uptime_ticks();

    if (_zros_sub_is_single_publisher(sub)) {
        _zros_sub_single_publisher_legacy_signal_consume(sub);
    }

    return _zros_sub_peek_update_available(sub, now_ticks);
}

int zros_sub_wait(struct zros_sub* sub, k_timeout_t timeout)
{
    k_timepoint_t deadline;

    __ASSERT(sub != NULL, "zros sub is null");
    __ASSERT(sub->_initialized, "zros sub not initialized");

    if (!_zros_sub_is_single_publisher(sub)) {
        return k_poll(&sub->_event, 1, timeout);
    }

    deadline = sys_timepoint_calc(timeout);

    while (true) {
        int64_t now_ticks = k_uptime_ticks();
        k_timeout_t remaining = sys_timepoint_timeout(deadline);
        k_timeout_t wait_timeout;
        uint32_t matched;
        uint32_t generation = (uint32_t)atomic_get(&sub->_topic->_lockless_generation);

        if (_zros_sub_single_publisher_ready(sub, now_ticks)) {
            return 0;
        }

        if (K_TIMEOUT_EQ(remaining, K_NO_WAIT)) {
            return -EAGAIN;
        }

        if (generation != sub->_last_seen_generation) {
            wait_timeout = _zros_sub_rate_limit_timeout(sub, now_ticks);
            if (K_TIMEOUT_EQ(wait_timeout, K_NO_WAIT)) {
                continue;
            }
            if (!K_TIMEOUT_EQ(remaining, K_FOREVER) && (K_TIMEOUT_EQ(wait_timeout, K_FOREVER) || wait_timeout.ticks > remaining.ticks)) {
                wait_timeout = remaining;
            }
        } else {
            wait_timeout = remaining;
        }

        matched = k_event_wait_safe(&sub->_topic->_data_event, ZROS_TOPIC_EVENT_UPDATED, false,
            wait_timeout);
        if (matched != 0U) {
            continue;
        }

        if (K_TIMEOUT_EQ(sys_timepoint_timeout(deadline), K_NO_WAIT)) {
            return -EAGAIN;
        }
    }
}

void zros_sub_fini(struct zros_sub* sub)
{
    __ASSERT(sub != NULL, "zros sub is null");
    __ASSERT(sub->_initialized, "zros sub not initialized");

    if (_zros_sub_is_single_publisher(sub) && sub->_legacy_poll_enabled) {
        atomic_dec(&sub->_topic->_legacy_poll_sub_count);
        sub->_legacy_poll_enabled = false;
    }

    zros_topic_remove_sub(sub->_topic, sub);
    sub->_initialized = false;
}

int zros_sub_wait_many(struct zros_sub* const* subs, size_t count, k_timeout_t timeout)
{
    k_timepoint_t deadline;

    __ASSERT(subs != NULL, "zros subs array is null");
    __ASSERT(count > 0U, "zros subs array must not be empty");

    if (count == 0U) {
        return -EINVAL;
    }

    if (count == 1U) {
        return zros_sub_wait(subs[0], timeout);
    }

    for (size_t i = 0; i < count; ++i) {
        __ASSERT(subs[i] != NULL, "zros sub entry is null");
        __ASSERT(subs[i]->_initialized, "zros sub entry not initialized");
    }

    deadline = sys_timepoint_calc(timeout);

    while (true) {
        int64_t now_ticks = k_uptime_ticks();
        k_timeout_t remaining;
        k_timeout_t wait_timeout;
        uint32_t matched;

        for (size_t i = 0; i < count; ++i) {
            if (_zros_sub_peek_update_available(subs[i], now_ticks)) {
                return 0;
            }
        }

        remaining = sys_timepoint_timeout(deadline);
        if (K_TIMEOUT_EQ(remaining, K_NO_WAIT)) {
            return -EAGAIN;
        }

        wait_timeout = remaining;
        for (size_t i = 0; i < count; ++i) {
            k_timeout_t candidate = _zros_sub_next_ready_timeout(subs[i], now_ticks);

            if (_zros_timeout_is_shorter(candidate, wait_timeout)) {
                wait_timeout = candidate;
            }
        }

        matched = k_event_wait_safe(&g_zros_wait_any_event, ZROS_WAIT_ANY_EVENT_UPDATED, false,
            wait_timeout);
        if (matched != 0U) {
            continue;
        }

        if (K_TIMEOUT_EQ(sys_timepoint_timeout(deadline), K_NO_WAIT)) {
            return -EAGAIN;
        }
    }
}

struct k_poll_event* zros_sub_get_event(struct zros_sub* sub)
{
    __ASSERT(sub != NULL, "zros sub is null");
    __ASSERT(sub->_initialized, "zros sub not initialized");

    if (_zros_sub_is_single_publisher(sub) && !sub->_legacy_poll_enabled) {
        sub->_legacy_poll_enabled = true;
        atomic_inc(&sub->_topic->_legacy_poll_sub_count);

        if ((uint32_t)atomic_get(&sub->_topic->_lockless_generation) != sub->_last_seen_generation) {
            k_poll_signal_raise(&sub->_data_ready, 1);
        }
    }

    return &sub->_event;
}

void zros_sub_get_node(struct zros_sub* sub, struct zros_node** node)
{
    __ASSERT(sub != NULL, "zros sub is null");
    __ASSERT(sub->_initialized, "zros sub not initialized");
    __ASSERT(node != NULL, "zros node output is null");
    *node = sub->_node;
};

// vi: ts=4 sw=4 et
