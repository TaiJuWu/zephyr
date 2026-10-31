/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ports of RIOT tests/core/thread_flags and tests/core/thread_flags_group.
 *
 * RIOT thread flags belong to a thread; here each thread gets a k_event:
 * - thread_flags_set(t, f)       k_event_post(&flags_of(t), f)
 * - thread_flags_wait_any(m)     k_event_wait_safe(m): clears what it returns
 * - thread_flags_wait_all(m)     k_event_wait_all_safe(m)
 * - thread_flags_wait_one(m)     no counterpart: wait for any without
 *                                clearing, then clear and return the lowest
 * - xtimer_set_timeout_flag()    a one-shot k_timer posting THREAD_FLAG_TIMEOUT
 * - a thread flags group         one k_event that the members wait on
 * The test scripts (tests/01-run.py) check the order of the printed lines;
 * here the lines go to a log that is compared with the expected one.
 */

#include <stdarg.h>
#include <zephyr/ztest.h>
#include "port.h"

#define THREAD_FLAG_TIMEOUT BIT(14)
#define STACK_SIZE          (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)
#define TIMEOUT_US          (100U * USEC_PER_MSEC)
#define THRESHOLD_US        500U

static K_THREAD_STACK_ARRAY_DEFINE(stacks, 3, STACK_SIZE);
static struct k_thread threads[3];

/* tests/core/thread_flags */

static struct k_event second_flags, main_flags;
static volatile bool done;
static char seq[512];
static size_t seq_len;

static void seq_add(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	seq_len += vsnprintk(&seq[seq_len], sizeof(seq) - seq_len, fmt, ap);
	va_end(ap);
}

static uint32_t wait_one(struct k_event *flags, uint32_t mask)
{
	uint32_t f = k_event_wait(flags, mask, false, K_FOREVER);
	uint32_t one = f & -f;

	k_event_clear(flags, one);
	return one;
}

static void second_thread(void *p1, void *p2, void *p3)
{
	uint32_t f;

	seq_add("w1;");
	f = k_event_wait_safe(&second_flags, 0x1, false, K_FOREVER);
	seq_add("r%x;", f);
	seq_add("w1|64;");
	f = k_event_wait_safe(&second_flags, 0x1 | 0x64, false, K_FOREVER);
	seq_add("r%x;", f);
	seq_add("w2&4;");
	f = k_event_wait_all_safe(&second_flags, 0x2 | 0x4, false, K_FOREVER);
	seq_add("r%x;", f);
	seq_add("w1by1;");
	f = wait_one(&second_flags, 0xFFFF);
	seq_add("r%x;", f);
	seq_add("w1by1;");
	f = wait_one(&second_flags, 0xFFFF);
	seq_add("r%x;", f);
	done = true;
}

static void set(struct k_event *flags, uint32_t f)
{
	seq_add("s%x;", f);
	k_event_post(flags, f);
}

static void timeout_expiry(struct k_timer *timer)
{
	k_event_post(&main_flags, THREAD_FLAG_TIMEOUT);
}

static K_TIMER_DEFINE(timeout_timer, timeout_expiry, NULL);

ZTEST(other_os_events, test_riot_thread_flags)
{
	static const char expected[] =
		"w1;s1;r1;w1|64;s64;r64;w2&4;s1;s8;s2;s4;r6;w1by1;r1;w1by1;r8;";
	int main_prio = PORT_MAIN_PRIO;

	k_thread_priority_set(k_current_get(), main_prio);
	k_event_init(&second_flags);
	k_event_init(&main_flags);
	done = false;
	seq_len = 0;

	k_thread_create(&threads[0], stacks[0], STACK_SIZE, second_thread, NULL, NULL, NULL,
			main_prio - 1, 0, K_NO_WAIT);
	set(&second_flags, 0x1);
	set(&second_flags, 0x64);
	set(&second_flags, 0x1);
	set(&second_flags, 0x8);
	set(&second_flags, 0x2);
	set(&second_flags, 0x4);
	zassert_true(done, "second thread not done (log %s)", seq);
	zassert_mem_equal(seq, expected, sizeof(expected) - 1, "log %s", seq);

	/* The 32-bit and 64-bit xtimer variants are the same here */
	for (int i = 0; i < 2; i++) {
		k_event_clear(&main_flags, ~0U);
		uint64_t before = k_ticks_to_us_floor64(k_uptime_ticks());

		k_timer_start(&timeout_timer, K_USEC(TIMEOUT_US), K_NO_WAIT);
		k_event_wait_safe(&main_flags, THREAD_FLAG_TIMEOUT, false, K_FOREVER);
		uint64_t diff = k_ticks_to_us_floor64(k_uptime_ticks()) - before;

		printk("timeout triggered after %llu us\n", diff);
		zassert_true(diff >= TIMEOUT_US, "timeout after %llu us", diff);
		zassert_true(diff < TIMEOUT_US + THRESHOLD_US, "timeout after %llu us", diff);
	}
}

/* tests/core/thread_flags_group */

#define WAITER_THREADS_CNT 3
#define GOOD_FLAG          0x2
#define BAD_FLAG           0x4

static struct k_event group, forever_flags;
static atomic_t woken_up;
static atomic_t last_prio;
static volatile int order_error;

static void forever_waiter(void *p1, void *p2, void *p3)
{
	/* Not in the group: its own flags never get set */
	k_event_wait_safe(&forever_flags, GOOD_FLAG | BAD_FLAG, false, K_FOREVER);
	order_error = 1;
}

static void waiter(void *p1, void *p2, void *p3)
{
	int prio = k_thread_priority_get(k_current_get());

	k_event_wait(&group, GOOD_FLAG, false, K_FOREVER);
	/* Woken in priority order: highest (lowest number) first */
	if (atomic_get(&last_prio) > prio) {
		order_error = 2;
	}
	atomic_set(&last_prio, prio);
	atomic_inc(&woken_up);
}

ZTEST(other_os_events, test_riot_thread_flags_group)
{
	int main_prio = PORT_MAIN_PRIO;
	unsigned int waiters_cnt = 0;

	k_thread_priority_set(k_current_get(), main_prio);

	k_event_init(&group);
	k_event_init(&forever_flags);
	atomic_set(&woken_up, 0);
	atomic_set(&last_prio, K_HIGHEST_THREAD_PRIO);
	order_error = 0;

	for (int i = 0; i < WAITER_THREADS_CNT; i++) {
		bool is_waiter = (i % 3) != 0;

		if (is_waiter) {
			waiters_cnt++;
		}
		k_thread_create(&threads[i], stacks[i], STACK_SIZE,
				is_waiter ? waiter : forever_waiter, NULL, NULL, NULL,
				main_prio - i - 1, 0, K_NO_WAIT);
	}

	/* This shouldn't wake up */
	k_event_post(&group, BAD_FLAG);
	zassert_equal(atomic_get(&woken_up), 0);

	k_event_post(&group, GOOD_FLAG);
	/* The waiters have higher priorities, so they must have finished */
	zassert_equal(atomic_get(&woken_up), waiters_cnt, "%ld of %u woken",
		      atomic_get(&woken_up), waiters_cnt);
	zassert_equal(order_error, 0, "order error %d", order_error);

	for (int i = 0; i < WAITER_THREADS_CNT; i++) {
		k_thread_abort(&threads[i]);
	}
}
