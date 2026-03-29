/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/private/zros_topic_struct.h>
#include <zros/zros_broker.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>
#include <zros/zros_topic.h>

struct sample_msg {
	uint32_t seq;
	int32_t value;
	uint8_t payload[8];
};

ZROS_TOPIC_DEFINE(sample, struct sample_msg);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(sample_single_pub, struct sample_msg);

struct zros_fixture {
	struct zros_node pub_node;
	struct zros_node sub_node;
	struct zros_pub pub;
	struct zros_sub sub;
	struct sample_msg pub_msg;
	struct sample_msg sub_msg;
};

static void fill_msg(struct sample_msg *msg, uint32_t seq, int32_t value)
{
	msg->seq = seq;
	msg->value = value;

	for (size_t i = 0; i < ARRAY_SIZE(msg->payload); ++i) {
		msg->payload[i] = (uint8_t)(seq + value + (int32_t)i);
	}
}

static void assert_msg_eq(const struct sample_msg *actual, const struct sample_msg *expected)
{
	zassert_equal(actual->seq, expected->seq, "seq mismatch");
	zassert_equal(actual->value, expected->value, "value mismatch");
	zassert_mem_equal(actual->payload, expected->payload, sizeof(actual->payload),
			  "payload mismatch");
}

static void fixture_init(struct zros_fixture *fixture, struct zros_topic *topic,
			 const char *node_prefix, double rate_limit_hz)
{
	char pub_name[32];
	char sub_name[32];

	memset(fixture, 0, sizeof(*fixture));
	snprintk(pub_name, sizeof(pub_name), "%s_pub", node_prefix);
	snprintk(sub_name, sizeof(sub_name), "%s_sub", node_prefix);
	zros_node_init(&fixture->pub_node, pub_name);
	zros_node_init(&fixture->sub_node, sub_name);

	zassert_ok(zros_pub_init(&fixture->pub, &fixture->pub_node, topic,
				 &fixture->pub_msg));
	zassert_ok(zros_sub_init(&fixture->sub, &fixture->sub_node, topic,
				 &fixture->sub_msg, rate_limit_hz));
}

static void fixture_fini(struct zros_fixture *fixture)
{
	zros_sub_fini(&fixture->sub);
	zros_pub_fini(&fixture->pub);
	zros_node_fini(&fixture->sub_node);
	zros_node_fini(&fixture->pub_node);
}

static void count_nodes(const struct zros_node *node, void *data)
{
	size_t *count = data;

	ARG_UNUSED(node);
	(*count)++;
}

static void count_pubs(const struct zros_pub *pub, void *data)
{
	size_t *count = data;

	ARG_UNUSED(pub);
	(*count)++;
}

static void exercise_topic_publish_and_read_round_trip(struct zros_topic *topic)
{
	struct sample_msg tx = {0};
	struct sample_msg rx = {0};

	fill_msg(&tx, 7U, -42);

	zassert_ok(zros_topic_publish(topic, &tx));
	zassert_ok(zros_topic_read(topic, &rx));
	assert_msg_eq(&rx, &tx);
}

static void exercise_pub_sub_update(struct zros_topic *topic, const char *node_prefix)
{
	struct zros_fixture fixture;

	fixture_init(&fixture, topic, node_prefix, 1.0e9);
	fill_msg(&fixture.pub_msg, 11U, 1234);
	k_sleep(K_MSEC(2));

	zassert_ok(zros_pub_update(&fixture.pub));
	zassert_ok(zros_sub_wait(&fixture.sub, K_MSEC(20)));
	zassert_ok(zros_sub_update(&fixture.sub));
	assert_msg_eq(&fixture.sub_msg, &fixture.pub_msg);

	fixture_fini(&fixture);
}

static void exercise_single_pub_sub_update(struct zros_topic *topic, const char *node_prefix)
{
	struct zros_fixture fixture;

	fixture_init(&fixture, topic, node_prefix, 1.0e9);
	fill_msg(&fixture.pub_msg, 21U, -7);

	zassert_ok(zros_pub_update(&fixture.pub));
	zassert_ok(zros_sub_wait(&fixture.sub, K_MSEC(20)));
	zassert_ok(zros_sub_update(&fixture.sub));
	assert_msg_eq(&fixture.sub_msg, &fixture.pub_msg);

	fixture_fini(&fixture);
}

ZTEST(zros_basic, test_topic_publish_and_read_round_trip)
{
	exercise_topic_publish_and_read_round_trip(&topic_sample);
}

ZTEST(zros_basic, test_pub_sub_update_copies_published_value)
{
	exercise_pub_sub_update(&topic_sample, "sample");
}

ZTEST(zros_basic, test_single_publisher_topic_publish_and_read_round_trip)
{
	exercise_topic_publish_and_read_round_trip(&topic_sample_single_pub);
}

ZTEST(zros_basic, test_single_publisher_pub_sub_wait_copies_published_value)
{
	exercise_single_pub_sub_update(&topic_sample_single_pub, "sample_single_pub");
}

ZTEST(zros_basic, test_wait_many_detects_mixed_backend_updates)
{
	struct zros_node current_pub_node;
	struct zros_node current_sub_node;
	struct zros_node single_pub_node;
	struct zros_node single_sub_node;
	struct zros_pub current_pub;
	struct zros_pub single_pub;
	struct zros_sub current_sub;
	struct zros_sub single_sub;
	struct sample_msg current_pub_msg = {0};
	struct sample_msg current_sub_msg = {0};
	struct sample_msg single_pub_msg = {0};
	struct sample_msg single_sub_msg = {0};
	struct zros_sub *subs[] = { &current_sub, &single_sub };

	memset(&current_pub, 0, sizeof(current_pub));
	memset(&single_pub, 0, sizeof(single_pub));
	memset(&current_sub, 0, sizeof(current_sub));
	memset(&single_sub, 0, sizeof(single_sub));

	zros_node_init(&current_pub_node, "wait_many_current_pub");
	zros_node_init(&current_sub_node, "wait_many_current_sub");
	zros_node_init(&single_pub_node, "wait_many_single_pub");
	zros_node_init(&single_sub_node, "wait_many_single_sub");

	zassert_ok(zros_pub_init(&current_pub, &current_pub_node, &topic_sample, &current_pub_msg));
	zassert_ok(zros_sub_init(&current_sub, &current_sub_node, &topic_sample, &current_sub_msg,
				 1.0e9));
	zassert_ok(zros_pub_init(&single_pub, &single_pub_node, &topic_sample_single_pub,
				 &single_pub_msg));
	zassert_ok(zros_sub_init(&single_sub, &single_sub_node, &topic_sample_single_pub,
				 &single_sub_msg, 1.0e9));

	fill_msg(&single_pub_msg, 31U, 3100);
	zassert_ok(zros_pub_update(&single_pub));
	zassert_ok(zros_sub_wait_many(subs, ARRAY_SIZE(subs), K_MSEC(20)));
	zassert_false(zros_sub_update_available(&current_sub),
		      "current topic should not report an update yet");
	zassert_ok(zros_sub_update(&single_sub));
	assert_msg_eq(&single_sub_msg, &single_pub_msg);

	fill_msg(&current_pub_msg, 41U, -4100);
	k_sleep(K_MSEC(2));
	zassert_ok(zros_pub_update(&current_pub));
	zassert_ok(zros_sub_wait_many(subs, ARRAY_SIZE(subs), K_MSEC(20)));
	zassert_false(zros_sub_update_available(&single_sub),
		      "single publisher topic should not report a second update");
	zassert_ok(zros_sub_update(&current_sub));
	assert_msg_eq(&current_sub_msg, &current_pub_msg);

	zros_sub_fini(&single_sub);
	zros_sub_fini(&current_sub);
	zros_pub_fini(&single_pub);
	zros_pub_fini(&current_pub);
	zros_node_fini(&single_sub_node);
	zros_node_fini(&single_pub_node);
	zros_node_fini(&current_sub_node);
	zros_node_fini(&current_pub_node);
}

ZTEST(zros_basic, test_single_publisher_newest_value_wins)
{
	struct zros_fixture fixture;
	struct sample_msg newer = {0};

	fixture_init(&fixture, &topic_sample_single_pub, "single_pub", 1.0e9);

	fill_msg(&fixture.pub_msg, 1U, 10);
	zassert_ok(zros_pub_update(&fixture.pub));

	fill_msg(&newer, 2U, 20);
	fixture.pub_msg = newer;
	zassert_ok(zros_pub_update(&fixture.pub));

	zassert_ok(zros_sub_wait(&fixture.sub, K_MSEC(20)));
	zassert_ok(zros_sub_update(&fixture.sub));
	assert_msg_eq(&fixture.sub_msg, &newer);

	fixture_fini(&fixture);
}

ZTEST(zros_basic, test_single_publisher_rejects_second_pub)
{
	struct zros_node node_a;
	struct zros_node node_b;
	struct zros_pub pub_a;
	struct zros_pub pub_b;
	struct sample_msg msg_a = {0};
	struct sample_msg msg_b = {0};
	size_t pub_count = 0U;

	memset(&pub_a, 0, sizeof(pub_a));
	memset(&pub_b, 0, sizeof(pub_b));
	zros_node_init(&node_a, "single_pub_a");
	zros_node_init(&node_b, "single_pub_b");

	zassert_ok(zros_pub_init(&pub_a, &node_a, &topic_sample_single_pub, &msg_a));
	zassert_equal(zros_pub_init(&pub_b, &node_b, &topic_sample_single_pub, &msg_b), -EALREADY,
		      "second publisher should be rejected");
	zassert_ok(zros_topic_iterate_pub(&topic_sample_single_pub, count_pubs, &pub_count));
	zassert_equal(pub_count, 1U, "single-publisher topic should have one registered pub");

	zros_pub_fini(&pub_a);
	zros_node_fini(&node_b);
	zros_node_fini(&node_a);
}

ZTEST(zros_basic, test_single_publisher_legacy_poll_event_still_works)
{
	struct zros_fixture fixture;
	struct k_poll_event *event;

	fixture_init(&fixture, &topic_sample_single_pub, "single_pub_legacy_event", 1.0e9);
	fill_msg(&fixture.pub_msg, 51U, 5100);

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	event = zros_sub_get_event(&fixture.sub);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

	zassert_not_null(event, "deprecated poll event should still be exposed");
	zassert_ok(zros_pub_update(&fixture.pub));
	zassert_ok(k_poll(event, 1, K_MSEC(20)));
	zassert_ok(zros_sub_update(&fixture.sub));
	assert_msg_eq(&fixture.sub_msg, &fixture.pub_msg);
	zassert_equal(k_poll(event, 1, K_NO_WAIT), -EAGAIN, "event should be reset after update");

	fixture_fini(&fixture);
}

ZTEST(zros_basic, test_broker_tracks_live_nodes)
{
	struct zros_fixture fixture;
	size_t baseline = 0;
	size_t during = 0;
	size_t after = 0;

	zassert_ok(zros_broker_iterate_nodes(count_nodes, &baseline));

	fixture_init(&fixture, &topic_sample, "broker", 1.0e9);
	zassert_ok(zros_broker_iterate_nodes(count_nodes, &during));
	zassert_equal(during, baseline + 2U, "broker did not track both test nodes");

	fixture_fini(&fixture);
	zassert_ok(zros_broker_iterate_nodes(count_nodes, &after));
	zassert_equal(after, baseline, "broker leaked nodes after cleanup");
}

ZTEST_SUITE(zros_basic, NULL, NULL, NULL, NULL, NULL);
