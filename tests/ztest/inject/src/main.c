/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/ztest_inject.h>

#define STACK_SIZE (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

static struct k_timer timer;
static atomic_t expiries;

static void timer_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	atomic_inc(&expiries);
}

static void start_timer(void)
{
	atomic_set(&expiries, 0);
	k_timer_init(&timer, timer_expiry, NULL);
	k_timer_start(&timer, K_MSEC(2), K_NO_WAIT);
}

/* A handler on TIMEOUT_INFLIGHT sees the timer's timeout before the timer
 * expires.
 */
static atomic_t hook_hits;
static atomic_t hook_expiries_seen;

static void count_hook(enum ztest_inject_point point, void *arg, void *user_data)
{
	ARG_UNUSED(point);

	/* Runs in the timer interrupt: record, check in the test */
	if (arg == user_data) {
		atomic_set(&hook_expiries_seen, atomic_get(&expiries));
		atomic_inc(&hook_hits);
	}
}

ZTEST(ztest_inject, test_handler_runs_before_timeout)
{
	atomic_set(&hook_hits, 0);
	atomic_set(&hook_expiries_seen, -1);
	ztest_inject_set(ZTEST_INJECT_TIMEOUT_INFLIGHT, count_hook, &timer.timeout);

	start_timer();
	k_msleep(50);
	ztest_inject_clear(ZTEST_INJECT_TIMEOUT_INFLIGHT);

	zassert_equal(atomic_get(&hook_hits), 1);
	zassert_equal(atomic_get(&hook_expiries_seen), 0, "hook ran after the expiry");
	zassert_equal(atomic_get(&expiries), 1);
}

/* The rest needs a second CPU to watch the one held at the point */
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)

/* Checker threads, one pinned to each CPU. Whichever is not on the CPU
 * held at the point runs the check, the one on the held CPU cannot.
 */
static K_THREAD_STACK_ARRAY_DEFINE(checker_stacks, CONFIG_MP_MAX_NUM_CPUS, STACK_SIZE);
static struct k_thread checker_threads[CONFIG_MP_MAX_NUM_CPUS];
static K_SEM_DEFINE(checker_done, 0, 1);
static atomic_t checker_claimed;
static atomic_t checkers_stop;

static void run_checkers(k_thread_entry_t entry, void *arg)
{
	unsigned int num_cpus = arch_num_cpus();

	atomic_set(&checker_claimed, 0);
	atomic_set(&checkers_stop, 0);
	k_sem_reset(&checker_done);

	for (unsigned int i = 0; i < num_cpus; i++) {
		k_thread_create(&checker_threads[i], checker_stacks[i], STACK_SIZE, entry, arg,
				INT_TO_POINTER(i), NULL, K_PRIO_PREEMPT(1), 0, K_FOREVER);
		zassert_ok(k_thread_cpu_pin(&checker_threads[i], (int)i));
		k_thread_start(&checker_threads[i]);
	}
}

static void stop_checkers(void)
{
	atomic_set(&checkers_stop, 1);
	for (unsigned int i = 0; i < arch_num_cpus(); i++) {
		zassert_ok(k_thread_join(&checker_threads[i], K_SECONDS(5)));
	}
}

static void skip_unless_smp(void)
{
	if (arch_num_cpus() < 2) {
		ztest_test_skip();
	}
}

#ifdef CONFIG_SCHED_IPI_SUPPORTED
static int this_cpu(void)
{
	return (int)arch_curr_cpu()->id;
}

/* With the timer's timeout in flight, every other CPU runs a function in
 * interrupt context before the timer expires.
 */
static atomic_t ipi_runs;
static atomic_t ipi_bad;

static void ipi_func(struct ztest_inject_ipi *ii, void *arg)
{
	if (!k_is_in_isr() || (this_cpu() == ii->src_cpu) || (arg != &timer.timeout) ||
	    (atomic_get(&expiries) != 0)) {
		atomic_inc(&ipi_bad);
	}
	atomic_inc(&ipi_runs);
}
#endif

ZTEST(ztest_inject, test_ipi_while_inflight)
{
	skip_unless_smp();
#ifdef CONFIG_SCHED_IPI_SUPPORTED
	static struct ztest_inject_ipi ii = {.func = ipi_func};

	atomic_set(&ipi_runs, 0);
	atomic_set(&ipi_bad, 0);
	ztest_inject_ipi_arm(&ii, ZTEST_INJECT_TIMEOUT_INFLIGHT, &timer.timeout);

	start_timer();
	k_msleep(50);
	ztest_inject_ipi_disarm(&ii, ZTEST_INJECT_TIMEOUT_INFLIGHT);

	zassert_true(ii.fired);
	zassert_false(ii.timed_out, "other CPUs did not take the IPI");
	zassert_equal(atomic_get(&ipi_runs), arch_num_cpus() - 1);
	zassert_equal(atomic_get(&ipi_bad), 0);
	zassert_equal(atomic_get(&expiries), 1);
#else
	ztest_test_skip();
#endif
}

/* The CPU announcing the timer is held with the timeout in flight: the
 * timer has not expired while it is held, and expires once released.
 */
static struct ztest_inject_park park;
static atomic_t park_expiries_seen;

static void park_checker(void *p1, void *p2, void *p3)
{
	int me = POINTER_TO_INT(p2);

	ARG_UNUSED(p1);
	ARG_UNUSED(p3);

	while (atomic_get(&checkers_stop) == 0) {
		if (ztest_inject_park_wait(&park, 0) && (park.cpu != me) &&
		    atomic_cas(&checker_claimed, 0, 1)) {
			k_busy_wait(5000);
			atomic_set(&park_expiries_seen, atomic_get(&expiries));
			ztest_inject_park_release(&park, ZTEST_INJECT_TIMEOUT_INFLIGHT);
			k_sem_give(&checker_done);
		}
		arch_nop();
	}
}

ZTEST(ztest_inject, test_park_inflight)
{
	skip_unless_smp();

	atomic_set(&park_expiries_seen, -1);
	ztest_inject_park_arm(&park, ZTEST_INJECT_TIMEOUT_INFLIGHT, &timer.timeout);
	run_checkers(park_checker, NULL);

	start_timer();
	zassert_ok(k_sem_take(&checker_done, K_SECONDS(5)), "nothing parked");
	k_msleep(10);
	stop_checkers();

	zassert_false(park.timed_out);
	zassert_equal(park.arg, &timer.timeout);
	zassert_equal(atomic_get(&park_expiries_seen), 0, "timer expired while held");
	zassert_equal(atomic_get(&expiries), 1);
}

/* Abort a sleeping thread while its timeout is held in flight on another
 * CPU: halt_thread() reaches HALT_WAIT, and once the timeout is let go the
 * abort and a join both complete.
 */
static struct k_thread victim;
static K_THREAD_STACK_DEFINE(victim_stack, STACK_SIZE);
static atomic_t halt_wait_hits;

static void victim_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_msleep(2);
	ztest_test_fail();
}

static void halt_wait_hook(enum ztest_inject_point point, void *arg, void *user_data)
{
	ARG_UNUSED(point);
	ARG_UNUSED(user_data);

	if (arg == &victim) {
		atomic_inc(&halt_wait_hits);
		/* The abort now waits for the timeout: let it go */
		ztest_inject_park_release(&park, ZTEST_INJECT_TIMEOUT_INFLIGHT);
	}
}

static void abort_checker(void *p1, void *p2, void *p3)
{
	int me = POINTER_TO_INT(p2);

	ARG_UNUSED(p1);
	ARG_UNUSED(p3);

	while (atomic_get(&checkers_stop) == 0) {
		if (ztest_inject_park_wait(&park, 0) && (park.cpu != me) &&
		    atomic_cas(&checker_claimed, 0, 1)) {
			k_thread_abort(&victim);
			k_sem_give(&checker_done);
		}
		arch_nop();
	}
}

ZTEST(ztest_inject, test_abort_while_timeout_inflight)
{
	skip_unless_smp();

	atomic_set(&halt_wait_hits, 0);
	ztest_inject_set(ZTEST_INJECT_HALT_WAIT, halt_wait_hook, NULL);
	ztest_inject_park_arm(&park, ZTEST_INJECT_TIMEOUT_INFLIGHT, &victim.base.timeout);

	k_thread_create(&victim, victim_stack, STACK_SIZE, victim_entry, NULL, NULL, NULL,
			K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	run_checkers(abort_checker, NULL);

	zassert_ok(k_sem_take(&checker_done, K_SECONDS(5)), "abort did not complete");
	stop_checkers();
	ztest_inject_clear(ZTEST_INJECT_HALT_WAIT);

	zassert_false(park.timed_out);
	zassert_equal(atomic_get(&halt_wait_hits), 1, "abort did not wait for the timeout");
	zassert_ok(k_thread_join(&victim, K_MSEC(100)));
}

#endif /* CONFIG_SMP && CONFIG_SCHED_CPU_MASK */

ZTEST_SUITE(ztest_inject, NULL, NULL, NULL, NULL, NULL);
