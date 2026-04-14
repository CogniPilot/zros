/*
 * Copyright (c) 2023 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZROS_PUB_H
#define ZROS_PUB_H

#include <zephyr/kernel.h>

/********************************************************************
 * zros pub
 ********************************************************************/
/* Public concrete type so applications can allocate publishers. */
struct zros_topic;
struct zros_node;

struct zros_pub {
    bool _initialized;
    sys_snode_t _topic_list_node;
    sys_snode_t _node_list_node;
    struct zros_topic* _topic;
    void* _data;
    struct zros_node* _node;
};

// public api
int zros_pub_init(struct zros_pub* pub, struct zros_node* node, struct zros_topic* topic, void* data);
int zros_pub_update(struct zros_pub* pub);
void zros_pub_fini(struct zros_pub* node);
void zros_pub_get_node(struct zros_pub* pub, struct zros_node** node);

// vi: ts=4 sw=4 et
#endif // ZROS_PUB_H
