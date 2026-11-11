/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest_inject.h>
#include <zephyr/sys/util.h>

struct inject_slot {
	atomic_ptr_t handler;
	void *user_data;
	/* CPUs currently inside the handler, for ztest_inject_clear() */
	atomic_t active;
};

static struct inject_slot slots[ZTEST_INJECT_POINT_COUNT];

void z_ztest_inject(enum ztest_inject_point point, void *arg)
{
	struct inject_slot *s = &slots[point];
	ztest_inject_handler_t handler;

	atomic_inc(&s->active);
	handler = (ztest_inject_handler_t)atomic_ptr_get(&s->handler);
	if (handler != NULL) {
		handler(point, arg, s->user_data);
	}
	atomic_dec(&s->active);
}

void ztest_inject_set(enum ztest_inject_point point, ztest_inject_handler_t handler,
		      void *user_data)
{
	__ASSERT_NO_MSG(point < ZTEST_INJECT_POINT_COUNT);

	ztest_inject_clear(point);
	slots[point].user_data = user_data;
	/* atomic_ptr_set() orders the user_data store before the handler */
	(void)atomic_ptr_set(&slots[point].handler, (atomic_ptr_val_t)handler);
}

void ztest_inject_clear(enum ztest_inject_point point)
{
	__ASSERT_NO_MSG(point < ZTEST_INJECT_POINT_COUNT);

	(void)atomic_ptr_clear(&slots[point].handler);
	while (atomic_get(&slots[point].active) != 0) {
		arch_nop();
	}
}

/* Spin, without sleeping or touching any kernel lock, until *v reads
 * @want. Returns false if CONFIG_ZTEST_INJECT_SPIN_TIMEOUT_MS passed first.
 * Usable at points that hold spinlocks or run with interrupts masked.
 */
static bool spin_for(atomic_t *v, atomic_val_t want, uint32_t timeout_ms)
{
	uint32_t start = k_cycle_get_32();
	uint32_t limit = (uint32_t)((uint64_t)timeout_ms * sys_clock_hw_cycles_per_sec() /
				    MSEC_PER_SEC);

	while (atomic_get(v) != want) {
		if ((k_cycle_get_32() - start) > limit) {
			return false;
		}
		arch_nop();
	}
	return true;
}

static int this_cpu(void)
{
#ifdef CONFIG_SMP
	return (int)arch_curr_cpu()->id;
#else
	return 0;
#endif
}

static void park_handler(enum ztest_inject_point point, void *arg, void *user_data)
{
	struct ztest_inject_park *pk = user_data;

	ARG_UNUSED(point);

	if (((pk->match != NULL) && (pk->match != arg)) || !atomic_cas(&pk->armed, 1, 0)) {
		return;
	}

	pk->cpu = this_cpu();
	pk->arg = arg;
	atomic_set(&pk->parked, 1);
	pk->timed_out = !spin_for(&pk->release, 1, CONFIG_ZTEST_INJECT_SPIN_TIMEOUT_MS);
	atomic_set(&pk->parked, 0);
}

void ztest_inject_park_arm(struct ztest_inject_park *pk, enum ztest_inject_point point,
			   const void *match)
{
	pk->match = match;
	pk->cpu = -1;
	pk->arg = NULL;
	pk->timed_out = false;
	atomic_set(&pk->parked, 0);
	atomic_set(&pk->release, 0);
	atomic_set(&pk->armed, 1);
	ztest_inject_set(point, park_handler, pk);
}

bool ztest_inject_park_wait(struct ztest_inject_park *pk, uint32_t timeout_ms)
{
	return spin_for(&pk->parked, 1, timeout_ms);
}

void ztest_inject_park_release(struct ztest_inject_park *pk, enum ztest_inject_point point)
{
	atomic_set(&pk->armed, 0);
	atomic_set(&pk->release, 1);
	ztest_inject_clear(point);
}

#ifdef CONFIG_SCHED_IPI_SUPPORTED

static struct ztest_inject_ipi *ipi_req;

/* SCHED_IPI: runs on every CPU taking an IPI, ours or not */
static void ipi_target_handler(enum ztest_inject_point point, void *arg, void *user_data)
{
	struct ztest_inject_ipi *ii = user_data;

	ARG_UNUSED(point);
	ARG_UNUSED(arg);

	if (atomic_test_and_clear_bit(&ii->todo, this_cpu())) {
		ii->func(ii, ii->arg);
		atomic_dec(&ii->pending);
	}
}

static void ipi_source_handler(enum ztest_inject_point point, void *arg, void *user_data)
{
	struct ztest_inject_ipi *ii = user_data;
	unsigned int num_cpus = arch_num_cpus();
	uint32_t others;

	ARG_UNUSED(point);

	if (((ii->match != NULL) && (ii->match != arg)) || !atomic_cas(&ii->armed, 1, 0)) {
		return;
	}

	ii->src_cpu = this_cpu();
	ii->arg = arg;
	others = BIT_MASK(num_cpus) & ~BIT(ii->src_cpu);
	atomic_set(&ii->pending, (atomic_val_t)(num_cpus - 1));
	ii->fired = true;
	/* Publishes arg and pending before any target can see its bit */
	atomic_set(&ii->todo, (atomic_val_t)others);

	if (others != 0U) {
#ifdef CONFIG_ARCH_HAS_DIRECTED_IPIS
		arch_sched_directed_ipi(others);
#else
		arch_sched_broadcast_ipi();
#endif
	}
	ii->timed_out = !spin_for(&ii->pending, 0, CONFIG_ZTEST_INJECT_SPIN_TIMEOUT_MS);
}

void ztest_inject_ipi_arm(struct ztest_inject_ipi *ii, enum ztest_inject_point point,
			  const void *match)
{
	__ASSERT_NO_MSG(ii->func != NULL);
	__ASSERT_NO_MSG(point != ZTEST_INJECT_SCHED_IPI);
	__ASSERT(ipi_req == NULL, "an IPI injection is already armed");

	ipi_req = ii;
	ii->match = match;
	ii->arg = NULL;
	ii->src_cpu = -1;
	ii->fired = false;
	ii->timed_out = false;
	atomic_set(&ii->todo, 0);
	atomic_set(&ii->pending, 0);
	atomic_set(&ii->armed, 1);
	ztest_inject_set(ZTEST_INJECT_SCHED_IPI, ipi_target_handler, ii);
	ztest_inject_set(point, ipi_source_handler, ii);
}

void ztest_inject_ipi_disarm(struct ztest_inject_ipi *ii, enum ztest_inject_point point)
{
	atomic_set(&ii->armed, 0);
	ztest_inject_clear(point);
	ztest_inject_clear(ZTEST_INJECT_SCHED_IPI);
	ipi_req = NULL;
}

#endif /* CONFIG_SCHED_IPI_SUPPORTED */
