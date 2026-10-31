/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Port of NuttX apps/testing/ostest/nxevent.c.
 *
 * NuttX -> Zephyr (NuttX sched/event):
 * - wait mask 0 means any event                   mask ~0
 * - nxevent_post(e, 0, ...) posts every event     k_event_post(~0)
 * - nxevent_post(..., 0)                          k_event_post()
 * - nxevent_post(..., NXEVENT_POST_SET)           k_event_set()
 * - nxevent_wait(..., 0): any, clear the matched  k_event_wait_safe()
 * - NXEVENT_WAIT_ALL                              k_event_wait_all_safe()
 * - NXEVENT_WAIT_NOCLEAR                          k_event_wait*() (not _safe)
 * - nxevent_trywait(), nxevent_tickwait(d)        K_NO_WAIT, K_TICKS(d)
 * - nxevent_clear(), nxevent_getmask()            k_event_clear(), k_event_test(~0)
 * Both return the matched events (all requested ones for WAIT_ALL).
 * The test's threads run at the default priority, as ostest itself does
 * by default, so they get the same priority as the ztest thread here;
 * nxevent_yield() is usleep(1).
 */

#include <zephyr/ztest.h>
#include "port.h"

#define NX_ANY      0x0
#define NX_ALL      0x1
#define NX_NOCLEAR  0x2
#define NX_POST_SET 0x1
#define STACK_SIZE  (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

static struct k_event event;
static K_THREAD_STACK_ARRAY_DEFINE(stacks, 2, STACK_SIZE);
static struct k_thread threads[2];
static volatile int nx_error_line;

static uint32_t nx_tickwait(struct k_event *ev, uint32_t events, int eflags,
			    k_timeout_t timeout)
{
	uint32_t m = (events == 0U) ? ~0U : events;

	if ((eflags & NX_ALL) != 0) {
		return (eflags & NX_NOCLEAR) ? k_event_wait_all(ev, m, false, timeout)
					     : k_event_wait_all_safe(ev, m, false, timeout);
	}
	return (eflags & NX_NOCLEAR) ? k_event_wait(ev, m, false, timeout)
				     : k_event_wait_safe(ev, m, false, timeout);
}

#define nx_wait(ev, m, f)    nx_tickwait(ev, m, f, K_FOREVER)
#define nx_trywait(ev, m, f) nx_tickwait(ev, m, f, K_NO_WAIT)

static void nx_post(struct k_event *ev, uint32_t events, int eflags)
{
	uint32_t e = (events == 0U) ? ~0U : events;

	if ((eflags & NX_POST_SET) != 0) {
		k_event_set(ev, e);
	} else {
		k_event_post(ev, e);
	}
}

static void nx_yield(void)
{
	k_usleep(1);
}

/* NXEVENT_TEST() asserts; keep the first failing line */
#define NXEVENT_TEST(expr, value)                                            \
	do {                                                                 \
		uint32_t r_ = (expr);                                        \
		if ((r_ != (uint32_t)(value)) && (nx_error_line == 0)) {     \
			printk("nxevent line %d: 0x%x != 0x%x\n", __LINE__, r_, \
			       (uint32_t)(value));                           \
			nx_error_line = __LINE__;                            \
		}                                                            \
	} while (false)

static void nxevent_test_rule1(struct k_event *ev)
{
	nx_post(ev, 0xf, 0);                            /* 2.1 */
	nx_yield();                                     /* 2.2 */
	NXEVENT_TEST(nx_wait(ev, 0, 0), ~0U);
	nx_yield();                                     /* 2.3 */
	nx_post(ev, ~0U, 0);
	nx_yield();                                     /* 2.4 */
	NXEVENT_TEST(nx_wait(ev, ~0U, 0), ~0U);
	nx_yield();                                     /* 2.5 */
	nx_post(ev, BIT(0), 0);
	nx_post(ev, BIT(1), 0);
	nx_post(ev, BIT(2), 0);
	nx_post(ev, BIT(3), 0);
	nx_yield();                                     /* 2.6 */
	nx_post(ev, 0xaaaaaaaa, 0);
	nx_yield();
	nx_yield();                                     /* 2.7 */
	nx_post(ev, 0, 0);
	nx_yield();
	nx_yield();
}

static void nxevent_test_rule0(struct k_event *ev)
{
	NXEVENT_TEST(nx_wait(ev, 0, 0), 0xf);           /* 2.1 */
	nx_yield();                                     /* 2.2 */
	nx_post(ev, ~0U, 0);
	nx_yield();                                     /* 2.3 */
	NXEVENT_TEST(nx_wait(ev, ~0U, 0), ~0U);
	nx_yield();                                     /* 2.4 */
	nx_post(ev, 0, 0);
	nx_yield();                                     /* 2.5 */
	NXEVENT_TEST(nx_wait(ev, 0xf, NX_ALL), 0xf);
	nx_yield();                                     /* 2.6 */
	NXEVENT_TEST(nx_wait(ev, 0xaaaaaaaa, 0), 0xaaaaaaaa);
	nx_yield();
	NXEVENT_TEST(nx_wait(ev, 0x1, 0), 0x1);         /* 2.7 */
	nx_yield();
	NXEVENT_TEST(nx_wait(ev, 0x2, 0), 0x2);
	NXEVENT_TEST(nx_wait(ev, 0, 0), ~(0x1U | 0x2U));
	nx_yield();
}

static void nxevent_thread(void *p1, void *p2, void *p3)
{
	struct k_event *ev = p1;

	nxevent_test_rule1(ev);
	nxevent_test_rule0(ev);
	nxevent_test_rule1(ev);
	nxevent_test_rule0(ev);
	nxevent_test_rule1(ev);
}

static void nxevent_thread1(void *p1, void *p2, void *p3)
{
	struct k_event *ev = p1;

	NXEVENT_TEST(nx_wait(ev, 0xf, 0), 0xf);         /* 3.1 */
	nx_yield();                                     /* 3.2 */
	nx_post(ev, 0xf, 0);
}

static void nxevent_thread2(void *p1, void *p2, void *p3)
{
	struct k_event *ev = p1;

	NXEVENT_TEST(nx_wait(ev, 0xf0, 0), 0xf0);       /* 3.1 */
	nx_yield();                                     /* 3.2 */
	nx_post(ev, 0xf0, 0);
}

static void start(int i, k_thread_entry_t entry)
{
	k_thread_create(&threads[i], stacks[i], STACK_SIZE, entry, &event, NULL, NULL,
			k_thread_priority_get(k_current_get()), 0, K_NO_WAIT);
}

static void join(int i)
{
	if (k_thread_join(&threads[i], K_SECONDS(5)) != 0) {
		k_thread_abort(&threads[i]);
		if (nx_error_line == 0) {
			nx_error_line = -1;
		}
	}
}

ZTEST(other_os_events, test_nuttx_nxevent)
{
	nx_error_line = 0;
	k_thread_priority_set(k_current_get(), PORT_MAIN_PRIO);

	/* NXEVENT_INITIALIZER(event, ~0) */
	k_event_init(&event);
	k_event_set(&event, ~0U);

	/* 1. Task local test */
	NXEVENT_TEST(nx_wait(&event, 0, 0), ~0U);       /* 1.1 */
	k_event_init(&event);                           /* 1.2 */
	k_event_set(&event, ~0U);
	NXEVENT_TEST(nx_wait(&event, ~0U, 0), ~0U);
	nx_post(&event, ~0U, 0);                        /* 1.3 */
	NXEVENT_TEST(nx_wait(&event, 0, 0), ~0U);
	nx_post(&event, ~0U, 0);                        /* 1.4 */
	NXEVENT_TEST(nx_wait(&event, ~0U, 0), ~0U);
	nx_post(&event, BIT(0), 0);                     /* 1.5 */
	nx_post(&event, BIT(1), 0);
	nx_post(&event, BIT(2), 0);
	nx_post(&event, BIT(3), 0);
	NXEVENT_TEST(nx_tickwait(&event, ~0U, NX_ALL, K_NO_WAIT), 0);
	NXEVENT_TEST(nx_trywait(&event, 0xf, 0), 0xf);  /* 1.6 */
	nx_post(&event, BIT(0), 0);
	nx_post(&event, BIT(1), 0);
	nx_post(&event, BIT(2), 0);
	nx_post(&event, BIT(3), 0);
	NXEVENT_TEST(nx_wait(&event, 0xf, 0), 0xf);     /* 1.7 */

	/* 2. Task notify test */
	start(0, nxevent_thread);
	nxevent_test_rule0(&event);
	nxevent_test_rule1(&event);
	nxevent_test_rule0(&event);
	nxevent_test_rule1(&event);
	nxevent_test_rule0(&event);
	join(0);

	/* 3. Multi task notify test */
	k_event_set(&event, 0);                         /* event.events = 0 */
	start(0, nxevent_thread1);
	start(1, nxevent_thread2);
	nx_yield();                                     /* 3.1 */
	nx_post(&event, 0xff, 0);
	nx_yield();
	NXEVENT_TEST(nx_wait(&event, 0xff, NX_ALL), 0xff); /* 3.2 */
	join(0);
	join(1);

	/* 4. Event clear test */
	nx_post(&event, 0xff, NX_POST_SET);
	k_event_clear(&event, 0);                       /* 4.1 */
	NXEVENT_TEST(nx_wait(&event, 0xf, NX_NOCLEAR), 0xf);
	NXEVENT_TEST(nx_trywait(&event, 0xf, NX_NOCLEAR), 0xf);
	k_event_clear(&event, 0xf);                     /* 4.2 */
	NXEVENT_TEST(nx_trywait(&event, 0xf, NX_NOCLEAR), 0);
	k_event_clear(&event, 0);                       /* 4.3 */
	NXEVENT_TEST(nx_wait(&event, 0xf0, NX_NOCLEAR), 0xf0);
	NXEVENT_TEST(nx_trywait(&event, 0xf0, NX_NOCLEAR), 0xf0);
	k_event_clear(&event, 0xf0);                    /* 4.4 */
	NXEVENT_TEST(nx_trywait(&event, 0xf0, NX_NOCLEAR), 0);

	/* 5. Event get mask test */
	nx_post(&event, 0xff, NX_POST_SET);             /* 5.1 */
	NXEVENT_TEST(k_event_test(&event, ~0U), 0xff);
	k_event_clear(&event, 0xf);                     /* 5.2 */
	NXEVENT_TEST(k_event_test(&event, ~0U), 0xf0);
	k_event_clear(&event, 0xf0);                    /* 5.3 */
	NXEVENT_TEST(k_event_test(&event, ~0U), 0);

	zassert_equal(nx_error_line, 0, "first failure at line %d (-1: a thread hung)",
		      nx_error_line);
}
