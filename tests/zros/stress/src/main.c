/*
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(CONFIG_ARCH_POSIX) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <limits.h>
#include <stdint.h>
#include <string.h>

#if defined(CONFIG_ARCH_POSIX)
#include <time.h>
#endif

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/time_units.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/private/zros_topic_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>
#include <zros/zros_topic.h>

struct bench_msg {
	uint32_t seq;
	uint32_t checksum;
	uint8_t payload[CONFIG_ZROS_STRESS_PAYLOAD_BYTES];
};

BUILD_ASSERT(CONFIG_ZROS_STRESS_PAYLOAD_BYTES > 0, "payload must not be empty");

ZROS_TOPIC_DEFINE(current_direct, struct bench_msg);
ZROS_TOPIC_DEFINE(current_pub, struct bench_msg);
ZROS_TOPIC_DEFINE(current_contended, struct bench_msg);
ZROS_TOPIC_DEFINE(current_multi_writer, struct bench_msg);
ZROS_TOPIC_DEFINE(current_subscribers, struct bench_msg);

ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(single_pub_direct, struct bench_msg);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(single_pub_pub, struct bench_msg);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(single_pub_contended, struct bench_msg);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(single_pub_multi_writer, struct bench_msg);
ZROS_TOPIC_DEFINE_SINGLE_PUBLISHER(single_pub_subscribers, struct bench_msg);

struct bench_backend {
	const char *label;
	struct zros_topic *direct_topic;
	struct zros_topic *pub_topic;
	struct zros_topic *contended_topic;
	struct zros_topic *multi_writer_topic;
	struct zros_topic *subscriber_topic;
};

static struct bench_backend g_backends[] = {
	{
		.label = "current",
		.direct_topic = &topic_current_direct,
		.pub_topic = &topic_current_pub,
		.contended_topic = &topic_current_contended,
		.multi_writer_topic = &topic_current_multi_writer,
		.subscriber_topic = &topic_current_subscribers,
	},
	{
		.label = "single_pub",
		.direct_topic = &topic_single_pub_direct,
		.pub_topic = &topic_single_pub_pub,
		.contended_topic = &topic_single_pub_contended,
		.multi_writer_topic = &topic_single_pub_multi_writer,
		.subscriber_topic = &topic_single_pub_subscribers,
	},
};

struct bench_stats {
	uint64_t total_ns;
	uint64_t min_ns;
	uint64_t max_ns;
	uint32_t samples;
	uint32_t errors;
};

struct pub_fixture {
	struct zros_node node;
	struct zros_pub pub;
	struct bench_msg msg;
};

struct subscriber_fixture {
	struct zros_node node;
	struct zros_sub subs[CONFIG_ZROS_STRESS_SUBSCRIBER_COUNT];
	struct bench_msg msgs[CONFIG_ZROS_STRESS_SUBSCRIBER_COUNT];
	size_t count;
};

struct reader_ctrl;

struct reader_ctx {
	struct reader_ctrl *ctrl;
	struct zros_topic *topic;
	atomic_t ops;
	atomic_t errors;
	struct k_thread thread;
};

struct reader_ctrl {
	atomic_t ready_count;
	atomic_t start_flag;
	atomic_t stop_flag;
	struct reader_ctx readers[CONFIG_ZROS_STRESS_READER_COUNT];
};

struct contended_ctx {
	const char *backend_label;
	struct pub_fixture fixture;
	struct reader_ctrl ctrl;
	struct bench_stats stats;
	struct k_sem done_sem;
	atomic_t completed_ops;
	struct k_thread publisher_thread;
};

struct multi_writer_ctrl;

struct multi_writer_ctx {
	struct multi_writer_ctrl *ctrl;
	struct zros_topic *topic;
	uint32_t writer_id;
	atomic_t errors;
	struct k_thread thread;
};

struct multi_writer_ctrl {
	atomic_t ready_count;
	atomic_t start_flag;
	atomic_t stop_flag;
	atomic_t done_count;
	struct multi_writer_ctx writers[CONFIG_ZROS_STRESS_WRITER_COUNT];
};

static struct subscriber_fixture g_subscriber_fixture;

K_THREAD_STACK_ARRAY_DEFINE(reader_stacks, CONFIG_ZROS_STRESS_READER_COUNT,
			    CONFIG_ZROS_STRESS_THREAD_STACK_SIZE);
K_THREAD_STACK_ARRAY_DEFINE(writer_stacks, CONFIG_ZROS_STRESS_WRITER_COUNT,
			    CONFIG_ZROS_STRESS_THREAD_STACK_SIZE);
K_THREAD_STACK_DEFINE(publisher_stack, CONFIG_ZROS_STRESS_THREAD_STACK_SIZE);

static void stats_init(struct bench_stats *stats)
{
	stats->total_ns = 0U;
	stats->min_ns = UINT64_MAX;
	stats->max_ns = 0U;
	stats->samples = 0U;
	stats->errors = 0U;
}

static void stats_add_batch(struct bench_stats *stats, uint64_t batch_ns,
			    uint32_t batch_samples, uint32_t batch_errors)
{
	uint64_t per_sample_ns;

	if (batch_samples == 0U) {
		return;
	}

	per_sample_ns = batch_ns / batch_samples;

	if (per_sample_ns < stats->min_ns) {
		stats->min_ns = per_sample_ns;
	}

	if (per_sample_ns > stats->max_ns) {
		stats->max_ns = per_sample_ns;
	}

	stats->total_ns += batch_ns;
	stats->samples += batch_samples;
	stats->errors += batch_errors;
}

static uint64_t stats_avg_ns(const struct bench_stats *stats)
{
	if (stats->samples == 0U) {
		return 0U;
	}

	return stats->total_ns / stats->samples;
}

static uint64_t stats_min_ns(const struct bench_stats *stats)
{
	if (stats->samples == 0U) {
		return 0U;
	}

	return stats->min_ns;
}

static uint64_t stats_max_ns(const struct bench_stats *stats)
{
	if (stats->samples == 0U) {
		return 0U;
	}

	return stats->max_ns;
}

static uint64_t now_ns(void)
{
#if defined(CONFIG_ARCH_POSIX)
	struct timespec ts;
	int rc;

	rc = clock_gettime(CLOCK_MONOTONIC, &ts);
	__ASSERT(rc == 0, "clock_gettime failed");
	if (rc != 0) {
		return 0U;
	}

	return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
#else
	return k_cyc_to_ns_floor64(k_cycle_get_64());
#endif
}

static void print_metric(const char *backend, const char *metric, uint32_t readers,
			 uint32_t subs, const struct bench_stats *stats, uint64_t aux_ops)
{
	printk("ZROS_BENCH backend=%s metric=%s bytes=%u readers=%u subs=%u avg_ns=%llu "
	       "min_ns=%llu max_ns=%llu iterations=%u errors=%u aux_ops=%llu\n",
	       backend, metric, CONFIG_ZROS_STRESS_PAYLOAD_BYTES, readers, subs,
	       (unsigned long long)stats_avg_ns(stats),
	       (unsigned long long)stats_min_ns(stats),
	       (unsigned long long)stats_max_ns(stats), stats->samples, stats->errors,
	       (unsigned long long)aux_ops);
}

static void fill_msg(struct bench_msg *msg, uint32_t seq)
{
	msg->seq = seq;
	msg->checksum = seq ^ CONFIG_ZROS_STRESS_PAYLOAD_BYTES;

	for (size_t i = 0; i < ARRAY_SIZE(msg->payload); ++i) {
		msg->payload[i] = (uint8_t)(seq + i);
	}
}

static int pub_fixture_init(struct pub_fixture *fixture, struct zros_topic *topic,
			    const char *name)
{
	memset(fixture, 0, sizeof(*fixture));
	zros_node_init(&fixture->node, name);
	return zros_pub_init(&fixture->pub, &fixture->node, topic, &fixture->msg);
}

static void pub_fixture_fini(struct pub_fixture *fixture)
{
	zros_pub_fini(&fixture->pub);
	zros_node_fini(&fixture->node);
}

static int subscriber_fixture_init(struct subscriber_fixture *fixture, struct zros_topic *topic,
				   const char *name)
{
	memset(fixture, 0, sizeof(*fixture));
	zros_node_init(&fixture->node, name);

	for (size_t i = 0; i < ARRAY_SIZE(fixture->subs); ++i) {
		int rc = zros_sub_init(&fixture->subs[i], &fixture->node, topic, &fixture->msgs[i],
				       1.0e9);

		if (rc < 0) {
			for (size_t j = 0; j < fixture->count; ++j) {
				zros_sub_fini(&fixture->subs[j]);
			}
			zros_node_fini(&fixture->node);
			return rc;
		}

		fixture->count++;
	}

	return 0;
}

static void subscriber_fixture_fini(struct subscriber_fixture *fixture)
{
	for (size_t i = 0; i < fixture->count; ++i) {
		zros_sub_fini(&fixture->subs[i]);
	}

	zros_node_fini(&fixture->node);
}

static uint32_t batch_count(uint32_t remaining)
{
	return MIN(remaining, (uint32_t)CONFIG_ZROS_STRESS_BATCH_SIZE);
}

static void reset_retry_counters(struct zros_topic *topic)
{
	atomic_set(&topic->_lockless_read_retries, 0);
	atomic_set(&topic->_lockless_write_retries, 0);
}

static uint64_t read_retry_count(const struct zros_topic *topic)
{
	return (uint64_t)atomic_get((atomic_t *)&topic->_lockless_read_retries);
}

static uint64_t write_retry_count(const struct zros_topic *topic)
{
	return (uint64_t)atomic_get((atomic_t *)&topic->_lockless_write_retries);
}

static uint64_t total_retry_count(const struct zros_topic *topic)
{
	return read_retry_count(topic) + write_retry_count(topic);
}

static void benchmark_topic_publish(const struct bench_backend *backend)
{
	struct bench_stats stats;
	struct bench_msg msg = {0};

	stats_init(&stats);
	reset_retry_counters(backend->direct_topic);

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_ITERATIONS;
	     i += batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i)) {
		uint32_t ops = batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i);
		uint32_t errors = 0U;
		uint64_t start = now_ns();
		uint64_t end;

		for (uint32_t j = 0; j < ops; ++j) {
			int rc;

			fill_msg(&msg, i + j);
			rc = zros_topic_publish(backend->direct_topic, &msg);
			if (rc < 0) {
				errors++;
			}
		}

		end = now_ns();
		stats_add_batch(&stats, end - start, ops, errors);
	}

	print_metric(backend->label, "topic_publish", 0U, 0U, &stats,
		     write_retry_count(backend->direct_topic));
}

static void benchmark_topic_read(const struct bench_backend *backend)
{
	struct bench_stats stats;
	struct bench_msg msg = {0};
	struct bench_msg rx = {0};

	stats_init(&stats);
	fill_msg(&msg, 0U);
	(void)zros_topic_publish(backend->direct_topic, &msg);
	reset_retry_counters(backend->direct_topic);

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_ITERATIONS;
	     i += batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i)) {
		uint32_t ops = batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i);
		uint32_t errors = 0U;
		uint64_t start = now_ns();
		uint64_t end;

		for (uint32_t j = 0; j < ops; ++j) {
			int rc = zros_topic_read(backend->direct_topic, &rx);

			if (rc < 0) {
				errors++;
			}
		}

		end = now_ns();
		stats_add_batch(&stats, end - start, ops, errors);
	}

	print_metric(backend->label, "topic_read", 0U, 0U, &stats,
		     read_retry_count(backend->direct_topic));
}

static void benchmark_topic_round_trip(const struct bench_backend *backend)
{
	struct bench_stats stats;
	struct bench_msg tx = {0};
	struct bench_msg rx = {0};

	stats_init(&stats);
	reset_retry_counters(backend->direct_topic);

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_ITERATIONS;
	     i += batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i)) {
		uint32_t ops = batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i);
		uint32_t errors = 0U;
		uint64_t start = now_ns();
		uint64_t end;

		for (uint32_t j = 0; j < ops; ++j) {
			int rc;

			fill_msg(&tx, i + j);
			rc = zros_topic_publish(backend->direct_topic, &tx);
			if (rc == 0) {
				rc = zros_topic_read(backend->direct_topic, &rx);
			}
			if (rc < 0) {
				errors++;
			}
		}

		end = now_ns();
		stats_add_batch(&stats, end - start, ops, errors);
	}

	print_metric(backend->label, "topic_round_trip", 0U, 0U, &stats,
		     total_retry_count(backend->direct_topic));
}

static int benchmark_pub_update(const struct bench_backend *backend)
{
	struct bench_stats stats;
	struct pub_fixture fixture;
	char node_name[32];
	int init_rc;

	stats_init(&stats);
	snprintk(node_name, sizeof(node_name), "%s_pub", backend->label);
	init_rc = pub_fixture_init(&fixture, backend->pub_topic, node_name);
	if (init_rc < 0) {
		return init_rc;
	}
	reset_retry_counters(backend->pub_topic);

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_ITERATIONS;
	     i += batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i)) {
		uint32_t ops = batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i);
		uint32_t errors = 0U;
		uint64_t start = now_ns();
		uint64_t end;

		for (uint32_t j = 0; j < ops; ++j) {
			int rc;

			fill_msg(&fixture.msg, i + j);
			rc = zros_pub_update(&fixture.pub);
			if (rc < 0) {
				errors++;
			}
		}

		end = now_ns();
		stats_add_batch(&stats, end - start, ops, errors);
	}

	print_metric(backend->label, "pub_update", 0U, 0U, &stats,
		     write_retry_count(backend->pub_topic));
	pub_fixture_fini(&fixture);
	return 0;
}

static int benchmark_pub_update_subscribers(const struct bench_backend *backend)
{
	struct bench_stats stats;
	struct pub_fixture pub_fixture;
	char pub_name[32];
	char sub_name[32];
	int rc;

	stats_init(&stats);
	snprintk(pub_name, sizeof(pub_name), "%s_pub_subs", backend->label);
	snprintk(sub_name, sizeof(sub_name), "%s_subs", backend->label);

	rc = pub_fixture_init(&pub_fixture, backend->subscriber_topic, pub_name);
	if (rc < 0) {
		return rc;
	}

	rc = subscriber_fixture_init(&g_subscriber_fixture, backend->subscriber_topic, sub_name);
	if (rc < 0) {
		pub_fixture_fini(&pub_fixture);
		return rc;
	}

	reset_retry_counters(backend->subscriber_topic);

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_ITERATIONS;
	     i += batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i)) {
		uint32_t ops = batch_count(CONFIG_ZROS_STRESS_ITERATIONS - i);
		uint32_t errors = 0U;
		uint64_t start = now_ns();
		uint64_t end;

		for (uint32_t j = 0; j < ops; ++j) {
			fill_msg(&pub_fixture.msg, i + j);
			rc = zros_pub_update(&pub_fixture.pub);
			if (rc < 0) {
				errors++;
			}
		}

		end = now_ns();
		stats_add_batch(&stats, end - start, ops, errors);
	}

	print_metric(backend->label, "pub_update_subscribers", 0U,
		     CONFIG_ZROS_STRESS_SUBSCRIBER_COUNT, &stats,
		     write_retry_count(backend->subscriber_topic));

	subscriber_fixture_fini(&g_subscriber_fixture);
	pub_fixture_fini(&pub_fixture);
	return 0;
}

static void reader_entry(void *arg1, void *arg2, void *arg3)
{
	struct reader_ctx *ctx = arg1;
	struct bench_msg msg = {0};
	uint32_t spins = 0U;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	atomic_inc(&ctx->ctrl->ready_count);

	while (atomic_get(&ctx->ctrl->start_flag) == 0) {
		k_yield();
	}

	while (atomic_get(&ctx->ctrl->stop_flag) == 0) {
		int rc = zros_topic_read(ctx->topic, &msg);

		if (rc == 0) {
			atomic_inc(&ctx->ops);
		} else {
			atomic_inc(&ctx->errors);
		}

		if (((++spins) & 0x1fU) == 0U) {
			k_yield();
		}
	}
}

static void publisher_entry(void *arg1, void *arg2, void *arg3)
{
	struct contended_ctx *ctx = arg1;

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_CONTENDED_ITERATIONS;
	     i += batch_count(CONFIG_ZROS_STRESS_CONTENDED_ITERATIONS - i)) {
		uint32_t ops = batch_count(CONFIG_ZROS_STRESS_CONTENDED_ITERATIONS - i);
		uint32_t errors = 0U;
		uint64_t start;
		uint64_t end;

		if (atomic_get(&ctx->ctrl.stop_flag) != 0) {
			break;
		}

		start = now_ns();

		for (uint32_t j = 0; j < ops; ++j) {
			int rc;

			if (atomic_get(&ctx->ctrl.stop_flag) != 0) {
				break;
			}

			fill_msg(&ctx->fixture.msg, i + j + 1U);
			rc = zros_pub_update(&ctx->fixture.pub);
			if (rc < 0) {
				errors++;
			}
			atomic_inc(&ctx->completed_ops);
		}

		end = now_ns();
		stats_add_batch(&ctx->stats, end - start, ops, errors);
	}

	k_sem_give(&ctx->done_sem);
}

static void multi_writer_entry(void *arg1, void *arg2, void *arg3)
{
	struct multi_writer_ctx *ctx = arg1;
	struct bench_msg msg = {0};

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	atomic_inc(&ctx->ctrl->ready_count);

	while (atomic_get(&ctx->ctrl->start_flag) == 0) {
		k_yield();
	}

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_MULTI_WRITER_ITERATIONS; ++i) {
		int rc;

		if (atomic_get(&ctx->ctrl->stop_flag) != 0) {
			break;
		}

		fill_msg(&msg, (ctx->writer_id << 24) | i);
		rc = zros_topic_publish(ctx->topic, &msg);
		if (rc < 0) {
			atomic_inc(&ctx->errors);
		}

		if ((i & 0x1fU) == 0U) {
			k_yield();
		}
	}

	atomic_inc(&ctx->ctrl->done_count);
}

static int benchmark_pub_update_contended(const struct bench_backend *backend)
{
	struct contended_ctx ctx;
	struct bench_stats reader_stats;
	uint64_t total_reader_ops = 0U;
	uint32_t total_reader_errors = 0U;
	bool timed_out = false;
	char node_name[32];
	int init_rc;

	memset(&ctx, 0, sizeof(ctx));
	ctx.backend_label = backend->label;
	stats_init(&ctx.stats);
	k_sem_init(&ctx.done_sem, 0, 1);

	snprintk(node_name, sizeof(node_name), "%s_contended", backend->label);
	init_rc = pub_fixture_init(&ctx.fixture, backend->contended_topic, node_name);
	if (init_rc < 0) {
		return init_rc;
	}

	fill_msg(&ctx.fixture.msg, 0U);
	(void)zros_pub_update(&ctx.fixture.pub);
	reset_retry_counters(backend->contended_topic);

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_READER_COUNT; ++i) {
		ctx.ctrl.readers[i].ctrl = &ctx.ctrl;
		ctx.ctrl.readers[i].topic = backend->contended_topic;
		atomic_set(&ctx.ctrl.readers[i].ops, 0);
		atomic_set(&ctx.ctrl.readers[i].errors, 0);

		k_thread_create(&ctx.ctrl.readers[i].thread, reader_stacks[i],
				K_THREAD_STACK_SIZEOF(reader_stacks[i]), reader_entry,
				&ctx.ctrl.readers[i], NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	}

	{
		uint64_t ready_deadline = now_ns() +
			((uint64_t)CONFIG_ZROS_STRESS_CONTENDED_TIMEOUT_MS * 1000000ULL);

		while (atomic_get(&ctx.ctrl.ready_count) < CONFIG_ZROS_STRESS_READER_COUNT &&
		       now_ns() < ready_deadline) {
			k_yield();
		}
	}

	if (atomic_get(&ctx.ctrl.ready_count) < CONFIG_ZROS_STRESS_READER_COUNT) {
		for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_READER_COUNT; ++i) {
			k_thread_abort(&ctx.ctrl.readers[i].thread);
		}
		ctx.stats.errors++;
		printk("ZROS_STRESS_WARN backend=%s metric=pub_update_contended stage=reader_start "
		       "ready=%ld\n",
		       backend->label, (long)atomic_get(&ctx.ctrl.ready_count));
		print_metric(backend->label, "pub_update_contended", CONFIG_ZROS_STRESS_READER_COUNT,
			     0U, &ctx.stats, total_retry_count(backend->contended_topic));
		pub_fixture_fini(&ctx.fixture);
		return 0;
	}

	atomic_set(&ctx.ctrl.start_flag, 1);
	for (uint32_t i = 0; i < 128U; ++i) {
		k_yield();
	}

	k_thread_create(&ctx.publisher_thread, publisher_stack,
			K_THREAD_STACK_SIZEOF(publisher_stack), publisher_entry, &ctx, NULL, NULL,
			K_PRIO_PREEMPT(0), 0, K_NO_WAIT);

	{
		uint64_t publisher_deadline = now_ns() +
			((uint64_t)CONFIG_ZROS_STRESS_CONTENDED_TIMEOUT_MS * 1000000ULL);

		while (k_sem_take(&ctx.done_sem, K_NO_WAIT) != 0) {
			if (now_ns() >= publisher_deadline) {
				timed_out = true;
				atomic_set(&ctx.ctrl.stop_flag, 1);
				break;
			}
			k_yield();
		}
	}

	atomic_set(&ctx.ctrl.stop_flag, 1);
	k_thread_join(&ctx.publisher_thread, K_FOREVER);

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_READER_COUNT; ++i) {
		k_thread_join(&ctx.ctrl.readers[i].thread, K_FOREVER);
		total_reader_ops += (uint64_t)atomic_get(&ctx.ctrl.readers[i].ops);
		total_reader_errors += (uint32_t)atomic_get(&ctx.ctrl.readers[i].errors);
	}

	ctx.stats.errors += total_reader_errors;

	if (timed_out) {
		ctx.stats.errors++;
		printk("ZROS_STRESS_WARN backend=%s metric=pub_update_contended timeout_ms=%u "
		       "completed_ops=%ld\n",
		       backend->label, CONFIG_ZROS_STRESS_CONTENDED_TIMEOUT_MS,
		       (long)atomic_get(&ctx.completed_ops));
	}

	print_metric(backend->label, "pub_update_contended", CONFIG_ZROS_STRESS_READER_COUNT,
		     0U, &ctx.stats, total_retry_count(backend->contended_topic));

	stats_init(&reader_stats);
	reader_stats.min_ns = 0U;
	reader_stats.max_ns = 0U;
	reader_stats.samples = (uint32_t)MIN(total_reader_ops, (uint64_t)UINT32_MAX);
	reader_stats.errors = total_reader_errors;
	print_metric(backend->label, "reader_progress_contended",
		     CONFIG_ZROS_STRESS_READER_COUNT, 0U, &reader_stats,
		     read_retry_count(backend->contended_topic));
	pub_fixture_fini(&ctx.fixture);
	return 0;
}

static int benchmark_topic_publish_multi_writer(const struct bench_backend *backend)
{
	struct multi_writer_ctrl ctrl;
	struct bench_stats stats;
	uint32_t total_errors = 0U;
	bool timed_out = false;
	uint64_t total_ops;
	uint64_t start;
	uint64_t end;

	memset(&ctrl, 0, sizeof(ctrl));
	stats_init(&stats);
	reset_retry_counters(backend->multi_writer_topic);

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_WRITER_COUNT; ++i) {
		ctrl.writers[i].ctrl = &ctrl;
		ctrl.writers[i].topic = backend->multi_writer_topic;
		ctrl.writers[i].writer_id = i + 1U;
		atomic_set(&ctrl.writers[i].errors, 0);

		k_thread_create(&ctrl.writers[i].thread, writer_stacks[i],
				K_THREAD_STACK_SIZEOF(writer_stacks[i]), multi_writer_entry,
				&ctrl.writers[i], NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	}

	{
		uint64_t ready_deadline = now_ns() +
			((uint64_t)CONFIG_ZROS_STRESS_CONTENDED_TIMEOUT_MS * 1000000ULL);

		while (atomic_get(&ctrl.ready_count) < CONFIG_ZROS_STRESS_WRITER_COUNT &&
		       now_ns() < ready_deadline) {
			k_yield();
		}
	}

	if (atomic_get(&ctrl.ready_count) < CONFIG_ZROS_STRESS_WRITER_COUNT) {
		for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_WRITER_COUNT; ++i) {
			k_thread_abort(&ctrl.writers[i].thread);
		}
		stats.errors++;
		printk("ZROS_STRESS_WARN backend=%s metric=topic_publish_multi_writer stage=writer_start "
		       "ready=%ld\n",
		       backend->label, (long)atomic_get(&ctrl.ready_count));
		print_metric(backend->label, "topic_publish_multi_writer",
			     CONFIG_ZROS_STRESS_WRITER_COUNT, 0U, &stats,
			     total_retry_count(backend->multi_writer_topic));
		return 0;
	}

	start = now_ns();
	atomic_set(&ctrl.start_flag, 1);

	{
		uint64_t done_deadline = now_ns() +
			((uint64_t)CONFIG_ZROS_STRESS_CONTENDED_TIMEOUT_MS * 1000000ULL);

		while (atomic_get(&ctrl.done_count) < CONFIG_ZROS_STRESS_WRITER_COUNT) {
			if (now_ns() >= done_deadline) {
				timed_out = true;
				atomic_set(&ctrl.stop_flag, 1);
				break;
			}
			k_yield();
		}
	}

	end = now_ns();

	for (uint32_t i = 0; i < CONFIG_ZROS_STRESS_WRITER_COUNT; ++i) {
		k_thread_join(&ctrl.writers[i].thread, K_FOREVER);
		total_errors += (uint32_t)atomic_get(&ctrl.writers[i].errors);
	}

	total_ops = (uint64_t)CONFIG_ZROS_STRESS_WRITER_COUNT *
		(uint64_t)CONFIG_ZROS_STRESS_MULTI_WRITER_ITERATIONS;
	stats_add_batch(&stats, end - start, (uint32_t)MIN(total_ops, (uint64_t)UINT32_MAX),
			total_errors);

	if (timed_out) {
		stats.errors++;
		printk("ZROS_STRESS_WARN backend=%s metric=topic_publish_multi_writer timeout_ms=%u "
		       "done=%ld\n",
		       backend->label, CONFIG_ZROS_STRESS_CONTENDED_TIMEOUT_MS,
		       (long)atomic_get(&ctrl.done_count));
	}

	print_metric(backend->label, "topic_publish_multi_writer",
		     CONFIG_ZROS_STRESS_WRITER_COUNT, 0U, &stats,
		     write_retry_count(backend->multi_writer_topic));
	return 0;
}

static int run_backend_benchmarks(const struct bench_backend *backend)
{
	int rc;

	benchmark_topic_publish(backend);
	benchmark_topic_read(backend);
	benchmark_topic_round_trip(backend);

	rc = benchmark_pub_update(backend);
	if (rc < 0) {
		return rc;
	}

	rc = benchmark_pub_update_subscribers(backend);
	if (rc < 0) {
		return rc;
	}

	rc = benchmark_topic_publish_multi_writer(backend);
	if (rc < 0) {
		return rc;
	}

	return benchmark_pub_update_contended(backend);
}

int main(void)
{
	printk("ZROS_STRESS_CONFIG bytes=%u iterations=%u batch_size=%u contended_iterations=%u "
	       "multi_writer_iterations=%u readers=%u writers=%u subscribers=%u writer_hold_yields=%u "
	       "reader_probe_yields=%u timeout_ms=%u backends=%u\n",
	       CONFIG_ZROS_STRESS_PAYLOAD_BYTES, CONFIG_ZROS_STRESS_ITERATIONS,
	       CONFIG_ZROS_STRESS_BATCH_SIZE, CONFIG_ZROS_STRESS_CONTENDED_ITERATIONS,
	       CONFIG_ZROS_STRESS_MULTI_WRITER_ITERATIONS, CONFIG_ZROS_STRESS_READER_COUNT,
	       CONFIG_ZROS_STRESS_WRITER_COUNT, CONFIG_ZROS_STRESS_SUBSCRIBER_COUNT,
	       CONFIG_ZROS_LOCKLESS_TEST_WRITER_HOLD_YIELDS,
	       CONFIG_ZROS_LOCKLESS_TEST_READER_PROBE_YIELDS,
	       CONFIG_ZROS_STRESS_CONTENDED_TIMEOUT_MS, ARRAY_SIZE(g_backends));

	for (size_t i = 0; i < ARRAY_SIZE(g_backends); ++i) {
		int rc = run_backend_benchmarks(&g_backends[i]);

		if (rc < 0) {
			printk("ZROS_STRESS_ERROR backend=%s rc=%d\n", g_backends[i].label, rc);
			return rc;
		}
	}

	printk("ZROS_STRESS_DONE\n");
	return 0;
}
