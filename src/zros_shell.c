/*
 * Copyright (c) 2026 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/types.h>
#include <zephyr/sys/util.h>

#include <zros/private/zros_topic_struct.h>
#include <zros/zros_broker.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_shell.h>
#include <zros/zros_sub.h>
#include <zros/zros_topic.h>

#define ZROS_SHELL_HEX_BYTES_PER_LINE 16U

enum zros_shell_watch_mode {
	ZROS_SHELL_WATCH_NONE = 0,
	ZROS_SHELL_WATCH_ECHO,
	ZROS_SHELL_WATCH_HZ,
};

struct zros_shell_watch {
	const struct shell* sh;
	struct zros_topic* topic;
	enum zros_shell_watch_mode mode;
	uint32_t period_ms;
	uint32_t last_generation;
	int64_t last_ms;
	bool active;
};

struct zros_shell_find_topic_ctx {
	const char* name;
	struct zros_topic* topic;
};

struct zros_shell_dynamic_topic_ctx {
	size_t target_idx;
	size_t current_idx;
	const char* name;
};

struct zros_shell_formatter_find_ctx {
	const struct zros_topic* topic;
	struct zros_shell_topic_formatter* formatter;
};

static sys_slist_t g_zros_shell_topic_formatters =
	SYS_SLIST_STATIC_INIT(g_zros_shell_topic_formatters);
static struct zros_shell_watch g_zros_shell_watch;
static union {
	z_max_align_t align;
	uint8_t bytes[CONFIG_ZROS_SHELL_MAX_TOPIC_SIZE];
} g_zros_shell_topic_storage;

K_MUTEX_DEFINE(g_zros_shell_formatter_lock);
K_SEM_DEFINE(g_zros_shell_watch_sem, 0, 1);

static void zros_shell_watch_thread(void* p0, void* p1, void* p2);
static void zros_shell_topic_name_dynamic_get(size_t idx, struct shell_static_entry* entry);

K_THREAD_DEFINE(g_zros_shell_watch_tid, CONFIG_ZROS_SHELL_THREAD_STACK_SIZE,
		zros_shell_watch_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
SHELL_DYNAMIC_CMD_CREATE(sub_zros_topic_names, zros_shell_topic_name_dynamic_get);

static void zros_shell_find_topic_cb(const struct zros_topic* topic, void* data)
{
	struct zros_shell_find_topic_ctx* ctx = data;

	if (ctx->topic == NULL && strcmp(topic->_name, ctx->name) == 0) {
		ctx->topic = (struct zros_topic*)topic;
	}
}

static struct zros_topic* zros_shell_find_topic_by_name(const char* name)
{
	struct zros_shell_find_topic_ctx ctx = {
		.name = name,
		.topic = NULL,
	};

	if (name == NULL) {
		return NULL;
	}

	(void)zros_broker_iterate_topic(zros_shell_find_topic_cb, &ctx);
	return ctx.topic;
}

static void zros_shell_dynamic_topic_name_cb(const struct zros_topic* topic, void* data)
{
	struct zros_shell_dynamic_topic_ctx* ctx = data;

	if (ctx->name != NULL) {
		return;
	}

	if (ctx->current_idx == ctx->target_idx) {
		ctx->name = topic->_name;
		return;
	}

	ctx->current_idx++;
}

static void zros_shell_topic_name_dynamic_get(size_t idx, struct shell_static_entry* entry)
{
	struct zros_shell_dynamic_topic_ctx ctx = {
		.target_idx = idx,
		.current_idx = 0U,
		.name = NULL,
	};

	(void)zros_broker_iterate_topic(zros_shell_dynamic_topic_name_cb, &ctx);
	if (ctx.name != NULL) {
		entry->syntax = ctx.name;
		entry->handler = NULL;
		entry->subcmd = NULL;
		entry->help = NULL;
	} else {
		entry->syntax = NULL;
	}
}

static struct zros_shell_topic_formatter* zros_shell_find_formatter(
	const struct zros_topic* topic)
{
	struct zros_shell_formatter_find_ctx ctx = {
		.topic = topic,
		.formatter = NULL,
	};
	sys_snode_t* node;

	k_mutex_lock(&g_zros_shell_formatter_lock, K_FOREVER);
	SYS_SLIST_FOR_EACH_NODE(&g_zros_shell_topic_formatters, node) {
		struct zros_shell_topic_formatter* formatter =
			CONTAINER_OF(node, struct zros_shell_topic_formatter, _node);

		if (formatter->topic == ctx.topic) {
			ctx.formatter = formatter;
			break;
		}
	}
	k_mutex_unlock(&g_zros_shell_formatter_lock);

	return ctx.formatter;
}

static void zros_shell_default_echo(const struct shell* sh, const struct zros_topic* topic,
				     const uint8_t* msg, size_t msg_size)
{
	for (size_t offset = 0U; offset < msg_size; offset += ZROS_SHELL_HEX_BYTES_PER_LINE) {
		char line[(ZROS_SHELL_HEX_BYTES_PER_LINE * 3U) + 1U];
		size_t line_len = 0U;
		size_t chunk_len = MIN(ZROS_SHELL_HEX_BYTES_PER_LINE, msg_size - offset);

		for (size_t i = 0U; i < chunk_len; ++i) {
			line_len += (size_t)snprintk(&line[line_len], sizeof(line) - line_len,
						     "%02x%s", msg[offset + i],
						     (i + 1U < chunk_len) ? " " : "");
		}

		shell_print(sh, "%s +0x%04x: %s", topic->_name, (unsigned int)offset, line);
	}
}

static int zros_shell_topic_echo_once(const struct shell* sh, struct zros_topic* topic)
{
	struct zros_shell_topic_formatter* formatter;
	uint32_t generation;

	if (topic == NULL) {
		return -EINVAL;
	}

	generation = (uint32_t)atomic_get(&topic->_lockless_generation);
	if (generation == 0U) {
		shell_print(sh, "%s: no samples", topic->_name);
		shell_print(sh, "");
		return 0;
	}

	if ((size_t)topic->_size > sizeof(g_zros_shell_topic_storage.bytes)) {
		shell_error(sh, "%s: topic size %d exceeds CONFIG_ZROS_SHELL_MAX_TOPIC_SIZE=%u",
			    topic->_name, topic->_size,
			    (unsigned int)sizeof(g_zros_shell_topic_storage.bytes));
		return -E2BIG;
	}

	if (zros_topic_read(topic, g_zros_shell_topic_storage.bytes) != 0) {
		shell_error(sh, "%s: read failed", topic->_name);
		return -EIO;
	}

	formatter = zros_shell_find_formatter(topic);
	if (formatter != NULL && formatter->format != NULL) {
		formatter->format(sh, topic, g_zros_shell_topic_storage.bytes,
				 (size_t)topic->_size);
		shell_print(sh, "");
		return 0;
	}

	zros_shell_default_echo(sh, topic, g_zros_shell_topic_storage.bytes,
			       (size_t)topic->_size);
	shell_print(sh, "");
	return 0;
}

static int zros_shell_topic_echo_refresh(const struct shell* sh, struct zros_topic* topic,
					 uint32_t period_ms)
{
	int rc;

	shell_fprintf(sh, SHELL_NORMAL, "\033[2J\033[H");
	rc = zros_shell_topic_echo_once(sh, topic);
	if (rc == 0) {
		shell_print(sh, "refresh every %u ms; Ctrl-C to stop",
			    (unsigned int)period_ms);
	}

	return rc;
}

static void zros_shell_watch_stop(void)
{
	unsigned int key = irq_lock();

	g_zros_shell_watch.active = false;
	g_zros_shell_watch.mode = ZROS_SHELL_WATCH_NONE;
	g_zros_shell_watch.topic = NULL;
	g_zros_shell_watch.sh = NULL;
	g_zros_shell_watch.period_ms = 0U;
	g_zros_shell_watch.last_generation = 0U;
	g_zros_shell_watch.last_ms = 0;

	irq_unlock(key);

	k_sem_give(&g_zros_shell_watch_sem);
}

void shell_ctrl_c(const struct shell* sh)
{
	unsigned int key = irq_lock();
	bool stop_watch = g_zros_shell_watch.active && g_zros_shell_watch.sh == sh;

	irq_unlock(key);

	if (!stop_watch) {
		return;
	}

	zros_shell_watch_stop();
}

static void zros_shell_watch_start(const struct shell* sh, struct zros_topic* topic,
				    enum zros_shell_watch_mode mode, uint32_t period_ms)
{
	unsigned int key = irq_lock();

	g_zros_shell_watch.sh = sh;
	g_zros_shell_watch.topic = topic;
	g_zros_shell_watch.mode = mode;
	g_zros_shell_watch.period_ms = period_ms;
	g_zros_shell_watch.last_generation = (uint32_t)atomic_get(&topic->_lockless_generation);
	g_zros_shell_watch.last_ms = k_uptime_get();
	g_zros_shell_watch.active = true;

	irq_unlock(key);

	k_sem_give(&g_zros_shell_watch_sem);
}

static void zros_shell_watch_thread(void* p0, void* p1, void* p2)
{
	struct zros_shell_watch watch;

	ARG_UNUSED(p0);
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);

	while (true) {
		(void)k_sem_take(&g_zros_shell_watch_sem, K_FOREVER);

		while (true) {
			uint32_t generation_now;
			int64_t now_ms;
			unsigned int key = irq_lock();

			watch = g_zros_shell_watch;
			irq_unlock(key);

			if (!watch.active || watch.sh == NULL || watch.topic == NULL) {
				break;
			}

			if (watch.mode == ZROS_SHELL_WATCH_ECHO) {
				(void)zros_shell_topic_echo_refresh(watch.sh, watch.topic,
							       watch.period_ms);
				if (k_sem_take(&g_zros_shell_watch_sem, K_MSEC(watch.period_ms)) == 0) {
					continue;
				}
			} else if (watch.mode == ZROS_SHELL_WATCH_HZ) {
				if (k_sem_take(&g_zros_shell_watch_sem, K_MSEC(watch.period_ms)) == 0) {
					continue;
				}

				key = irq_lock();
				watch = g_zros_shell_watch;
				irq_unlock(key);

				if (!watch.active || watch.sh == NULL || watch.topic == NULL) {
					break;
				}

				generation_now = (uint32_t)atomic_get(&watch.topic->_lockless_generation);
				now_ms = k_uptime_get();
				if (now_ms <= watch.last_ms) {
					now_ms = watch.last_ms + 1;
				}

				shell_print(watch.sh,
					    "%s: %u samples in %lld ms = %0.2f Hz",
					    watch.topic->_name,
					    (unsigned int)(generation_now -
							   watch.last_generation),
					    (long long)(now_ms - watch.last_ms),
					    ((double)(generation_now - watch.last_generation) *
					     1000.0) /
						    (double)(now_ms - watch.last_ms));

				key = irq_lock();
				if (g_zros_shell_watch.active &&
				    g_zros_shell_watch.mode == ZROS_SHELL_WATCH_HZ &&
				    g_zros_shell_watch.topic == watch.topic) {
					g_zros_shell_watch.last_generation = generation_now;
					g_zros_shell_watch.last_ms = now_ms;
				}
				irq_unlock(key);
			} else {
				break;
			}
		}
	}
}

static int zros_shell_parse_u32_arg(const char* arg, uint32_t* out)
{
	char* end = NULL;
	long value = strtol(arg, &end, 10);

	if (*arg == '\0' || *end != '\0' || value <= 0 || value > INT32_MAX) {
		return -EINVAL;
	}

	*out = (uint32_t)value;
	return 0;
}

static void zros_shell_append_name(char* buf, size_t buf_size, size_t* len,
				   bool* first, const char* name)
{
	int rc;

	if (buf == NULL || len == NULL || first == NULL || name == NULL || *len >= buf_size) {
		return;
	}

	rc = snprintk(buf + *len, buf_size - *len, "%s%s", *first ? "" : ",", name);
	if (rc <= 0) {
		return;
	}

	if ((size_t)rc >= (buf_size - *len)) {
		*len = buf_size - 1U;
	} else {
		*len += (size_t)rc;
	}
	*first = false;
}

static void zros_shell_node_topic_lists(const struct zros_node* node, char* pubs_buf,
					size_t pubs_buf_size, char* subs_buf,
					size_t subs_buf_size, size_t* pub_count,
					size_t* sub_count)
{
	struct zros_pub* pub;
	struct zros_sub* sub;
	size_t pubs_len = 0U;
	size_t subs_len = 0U;
	bool first_pub = true;
	bool first_sub = true;

	if (pubs_buf != NULL && pubs_buf_size > 0U) {
		pubs_buf[0] = '\0';
	}
	if (subs_buf != NULL && subs_buf_size > 0U) {
		subs_buf[0] = '\0';
	}
	if (pub_count != NULL) {
		*pub_count = 0U;
	}
	if (sub_count != NULL) {
		*sub_count = 0U;
	}

	(void)k_mutex_lock((struct k_mutex*)&node->_lock, K_FOREVER);
	SYS_SLIST_FOR_EACH_CONTAINER(&node->_pubs, pub, _node_list_node) {
		if (pub_count != NULL) {
			(*pub_count)++;
		}
		if (pub->_topic != NULL) {
			zros_shell_append_name(pubs_buf, pubs_buf_size, &pubs_len, &first_pub,
					       pub->_topic->_name);
		}
	}

	SYS_SLIST_FOR_EACH_CONTAINER(&node->_subs, sub, _node_list_node) {
		if (sub_count != NULL) {
			(*sub_count)++;
		}
		if (sub->_topic != NULL) {
			zros_shell_append_name(subs_buf, subs_buf_size, &subs_len, &first_sub,
					       sub->_topic->_name);
		}
	}
	k_mutex_unlock((struct k_mutex*)&node->_lock);
}

static void zros_shell_topic_list_cb(const struct zros_topic* topic, void* data)
{
	const struct shell* sh = data;

	shell_print(sh, "%s size=%d pubs=%u subs=%u gen=%u backend=%s",
		    topic->_name, topic->_size,
		    (unsigned int)atomic_get((atomic_t*)&topic->_pub_count),
		    (unsigned int)atomic_get((atomic_t*)&topic->_sub_count),
		    (unsigned int)atomic_get((atomic_t*)&topic->_lockless_generation),
		    topic->_backend == ZROS_TOPIC_BACKEND_SINGLE_PUBLISHER ? "latest" :
								       "mutex");
}

static int cmd_zros_topic_list(const struct shell* sh, size_t argc, char** argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return zros_broker_iterate_topic(zros_shell_topic_list_cb, (void*)sh);
}

static void zros_shell_node_list_cb(const struct zros_node* node, void* data)
{
	const struct shell* sh = data;
	char pubs[128];
	char subs[128];
	size_t pub_count;
	size_t sub_count;

	zros_shell_node_topic_lists(node, pubs, sizeof(pubs), subs, sizeof(subs), &pub_count,
				    &sub_count);

	shell_print(sh, "%s pubs=%u[%s] subs=%u[%s]",
		    node->_name,
		    (unsigned int)pub_count,
		    pub_count > 0U ? pubs : "-",
		    (unsigned int)sub_count,
		    sub_count > 0U ? subs : "-");
}

static int cmd_zros_node_list(const struct shell* sh, size_t argc, char** argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return zros_broker_iterate_nodes(zros_shell_node_list_cb, (void*)sh);
}

static int cmd_zros_topic_info(const struct shell* sh, size_t argc, char** argv)
{
	struct zros_topic* topic = zros_shell_find_topic_by_name(argv[1]);

	ARG_UNUSED(argc);

	if (topic == NULL) {
		shell_error(sh, "unknown topic: %s", argv[1]);
		return -ENOENT;
	}

	shell_print(sh, "name=%s size=%d pubs=%u subs=%u gen=%u backend=%s",
		    topic->_name, topic->_size,
		    (unsigned int)atomic_get(&topic->_pub_count),
		    (unsigned int)atomic_get(&topic->_sub_count),
		    (unsigned int)atomic_get(&topic->_lockless_generation),
		    topic->_backend == ZROS_TOPIC_BACKEND_SINGLE_PUBLISHER ? "latest" :
								       "mutex");
	return 0;
}

static int cmd_zros_topic_echo(const struct shell* sh, size_t argc, char** argv)
{
	struct zros_topic* topic = zros_shell_find_topic_by_name(argv[1]);
	uint32_t period_ms = 1000U;
	int rc;

	if (topic == NULL) {
		shell_error(sh, "unknown topic: %s", argv[1]);
		return -ENOENT;
	}

	if (argc >= 3) {
		rc = zros_shell_parse_u32_arg(argv[2], &period_ms);
		if (rc != 0) {
			shell_error(sh, "period_ms must be a positive integer");
			return rc;
		}
	}

	zros_shell_watch_start(sh, topic, ZROS_SHELL_WATCH_ECHO, period_ms);
	shell_print(sh, "echoing %s every %u ms; use 'zros topic stop' to stop",
		    topic->_name, (unsigned int)period_ms);
	return 0;
}

static int cmd_zros_topic_hz(const struct shell* sh, size_t argc, char** argv)
{
	struct zros_topic* topic = zros_shell_find_topic_by_name(argv[1]);
	uint32_t period_ms = 1000U;
	int rc;

	if (topic == NULL) {
		shell_error(sh, "unknown topic: %s", argv[1]);
		return -ENOENT;
	}

	if (argc >= 3) {
		rc = zros_shell_parse_u32_arg(argv[2], &period_ms);
		if (rc != 0) {
			shell_error(sh, "window_ms must be a positive integer");
			return rc;
		}
	}

	zros_shell_watch_start(sh, topic, ZROS_SHELL_WATCH_HZ, period_ms);
	shell_print(sh, "measuring %s every %u ms; use 'zros topic stop' to stop",
		    topic->_name, (unsigned int)period_ms);
	return 0;
}

static int cmd_zros_topic_stop(const struct shell* sh, size_t argc, char** argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	zros_shell_watch_stop();
	shell_print(sh, "zros topic watcher stopped");
	return 0;
}

int zros_shell_topic_formatter_register(struct zros_shell_topic_formatter* formatter)
{
	struct zros_shell_topic_formatter* existing;

	if (formatter == NULL || formatter->topic == NULL || formatter->format == NULL) {
		return -EINVAL;
	}

	existing = zros_shell_find_formatter(formatter->topic);
	if (existing != NULL) {
		return -EALREADY;
	}

	k_mutex_lock(&g_zros_shell_formatter_lock, K_FOREVER);
	formatter->_node.next = NULL;
	sys_slist_append(&g_zros_shell_topic_formatters, &formatter->_node);
	k_mutex_unlock(&g_zros_shell_formatter_lock);
	return 0;
}

int zros_shell_topic_formatter_unregister(struct zros_shell_topic_formatter* formatter)
{
	if (formatter == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&g_zros_shell_formatter_lock, K_FOREVER);
	(void)sys_slist_find_and_remove(&g_zros_shell_topic_formatters, &formatter->_node);
	k_mutex_unlock(&g_zros_shell_formatter_lock);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zros_node,
	SHELL_CMD(list, NULL, "list zros nodes", cmd_zros_node_list),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zros_topic,
	SHELL_CMD(list, NULL, "list zros topics", cmd_zros_topic_list),
	SHELL_CMD_ARG(info, &sub_zros_topic_names, "show topic info: zros topic info <name>",
		      cmd_zros_topic_info, 2, 0),
	SHELL_CMD_ARG(echo, &sub_zros_topic_names,
		      "echo topic: zros topic echo <name> [period_ms]",
		      cmd_zros_topic_echo, 2, 1),
	SHELL_CMD_ARG(hz, &sub_zros_topic_names,
		      "measure topic rate: zros topic hz <name> [window_ms]",
		      cmd_zros_topic_hz, 2, 1),
	SHELL_CMD(stop, NULL, "stop topic echo/hz", cmd_zros_topic_stop),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zros,
	SHELL_CMD(node, &sub_zros_node, "zros node commands", NULL),
	SHELL_CMD(topic, &sub_zros_topic, "zros topic commands", NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(zros, &sub_zros, "zros diagnostics", NULL);
