/*
 * Copyright (c) 2023 CogniPilot Foundation
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <zephyr/sys/__assert.h>
#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_broker.h>
#include <zros/zros_common.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zros);

static const k_timeout_t g_node_timeout = K_MSEC(1);

/********************************************************************
 * zros node
 ********************************************************************/
void zros_node_init(struct zros_node* node, const char* name)
{
    __ASSERT(node != NULL, "zros node is null");
    __ASSERT(name != NULL, "zros name is null");
    node->_name = name;
    node->_broker_list_node.next = NULL;
    sys_slist_init(&node->_subs);
    sys_slist_init(&node->_pubs);
    k_mutex_init(&node->_lock);
    zros_broker_add_node(node);
};

int _zros_node_lock(struct zros_node* node)
{
    __ASSERT(node != NULL, "zros node is null");
    return k_mutex_lock(&node->_lock, g_node_timeout);
}

void _zros_node_unlock(struct zros_node* node)
{
    __ASSERT(node != NULL, "zros node is null");
    k_mutex_unlock(&node->_lock);
}

int zros_node_add_sub(struct zros_node* node, struct zros_sub* sub)
{
    __ASSERT(node != NULL, "zros node is null");
    __ASSERT(sub != NULL, "zros sub is null");
    ZROS_RC(_zros_node_lock(node), return rc);
    sys_slist_append(&node->_subs, &sub->_node_list_node);
    _zros_node_unlock(node);
    return ZROS_OK;
}

int zros_node_add_pub(struct zros_node* node, struct zros_pub* pub)
{
    __ASSERT(node != NULL, "zros node is null");
    __ASSERT(pub != NULL, "zros pub is null");
    ZROS_RC(_zros_node_lock(node), return rc);
    sys_slist_append(&node->_pubs, &pub->_node_list_node);
    _zros_node_unlock(node);
    return ZROS_OK;
}

void zros_node_fini(struct zros_node* node)
{
    __ASSERT(node != NULL, "zros node is null");
    zros_broker_remove_node(node);
};

int zros_node_get_name(const struct zros_node* node, char* buf, size_t n)
{
    __ASSERT(node != NULL, "zros node is null");
    __ASSERT(buf != NULL, "zros buf is null");
    ZROS_RC(_zros_node_lock((struct zros_node*)node), return rc);
    ZROS_RC(snprintf(buf, n, "%s", node->_name), return rc);
    _zros_node_unlock((struct zros_node*)node);
    return ZROS_OK;
};

// vi: ts=4 sw=4 et
