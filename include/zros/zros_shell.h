/*
 * Copyright (c) 2026 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZROS_SHELL_H
#define ZROS_SHELL_H

#include <zephyr/kernel.h>

struct shell;
struct zros_topic;

typedef void zros_shell_topic_format_t(const struct shell* sh,
				       const struct zros_topic* topic,
				       const void* msg, size_t msg_size);

struct zros_shell_topic_formatter {
	struct zros_topic* topic;
	zros_shell_topic_format_t* format;
	sys_snode_t _node;
};

int zros_shell_topic_formatter_register(struct zros_shell_topic_formatter* formatter);
int zros_shell_topic_formatter_unregister(struct zros_shell_topic_formatter* formatter);

#endif // ZROS_SHELL_H
// vi: ts=4 sw=4 et
