/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Port of RT-Thread src/utest/event_tc.c, test_static_event_send_recv:
 * a receiver takes flag 3 with OR | CLEAR, sleeps while the sender posts
 * flag 5 and flag 3 again, then takes both with AND | CLEAR.
 *
 * RT-Thread -> Zephyr:
 * - rt_event_recv(OR | CLEAR)   k_event_wait_safe()
 * - rt_event_recv(AND | CLEAR)  k_event_wait_all_safe()
 * - rt_event_send()             k_event_post()
 * rt_event_recv() returns the matched flags, as k_event_wait*() does.
 * The init/detach/create/delete cases have no Zephyr counterpart beyond
 * k_event_init() (covered by tests/kernel/events) and are left out; the
 * dynamic variant is the same test on a heap-allocated event.
 */

#include <zephyr/ztest.h>
#include "port.h"

#define EVENT_FLAG3     BIT(3)
#define EVENT_FLAG5     BIT(5)
#define THREAD_PRIORITY 9
#define STACK_SIZE      (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

static struct k_event static_event;
static K_THREAD_STACK_DEFINE(thread1_stack, STACK_SIZE);
static K_THREAD_STACK_DEFINE(thread2_stack, STACK_SIZE);
static struct k_thread thread1, thread2;
static volatile uint32_t recv_event_times1, recv_event_times2;
static volatile bool recv_finish, send_finish;

static void thread1_recv_static_event(void *p1, void *p2, void *p3)
{
	uint32_t e;

	e = k_event_wait_safe(&static_event, EVENT_FLAG3 | EVENT_FLAG5, false, K_FOREVER);
	if (e == 0) {
		return;
	}
	recv_event_times1 = e;

	k_msleep(50);

	e = k_event_wait_all_safe(&static_event, EVENT_FLAG3 | EVENT_FLAG5, false, K_FOREVER);
	if (e == 0) {
		return;
	}
	recv_event_times2 = e;

	recv_finish = true;
}

static void thread2_send_static_event(void *p1, void *p2, void *p3)
{
	k_event_post(&static_event, EVENT_FLAG3);
	k_msleep(10);

	k_event_post(&static_event, EVENT_FLAG5);
	k_msleep(10);

	k_event_post(&static_event, EVENT_FLAG3);

	send_finish = true;
}

ZTEST(other_os_events, test_rtthread_event_send_recv)
{
	k_thread_priority_set(k_current_get(), PORT_MAIN_PRIO);
	k_event_init(&static_event);
	recv_event_times1 = 0;
	recv_event_times2 = 0;
	recv_finish = false;
	send_finish = false;

	k_thread_create(&thread1, thread1_stack, STACK_SIZE, thread1_recv_static_event,
			NULL, NULL, NULL, K_PRIO_PREEMPT(THREAD_PRIORITY - 1), 0, K_NO_WAIT);
	k_thread_create(&thread2, thread2_stack, STACK_SIZE, thread2_send_static_event,
			NULL, NULL, NULL, K_PRIO_PREEMPT(THREAD_PRIORITY), 0, K_NO_WAIT);

	/* The utest polls with rt_thread_delay(1); bound it here */
	for (int i = 0; i < 1000 && !(recv_finish && send_finish); i++) {
		k_sleep(K_TICKS(1));
	}

	k_thread_abort(&thread1);
	k_thread_abort(&thread2);

	zassert_true(recv_finish && send_finish, "threads did not finish");
	zassert_equal(recv_event_times1, EVENT_FLAG3, "first recv 0x%x", recv_event_times1);
	zassert_equal(recv_event_times2, EVENT_FLAG3 | EVENT_FLAG5, "second recv 0x%x",
		      recv_event_times2);
}
