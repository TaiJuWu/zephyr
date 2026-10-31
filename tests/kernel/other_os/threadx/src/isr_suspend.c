/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ports of:
 * - threadx_event_flag_isr_set_clear_test.c: an ISR sets and clears flags
 *   while threads set and wait on the same group; no wakeup may be lost.
 * - threadx_thread_delayed_suspension_test.c, first part: suspending a thread
 *   that is blocked on a semaphore, then giving and resuming it.
 *
 * ThreadX calls the test ISR from its periodic timer interrupt; here a
 * one-tick k_timer does. See README.rst for what could not be kept.
 */

#include <wait_q.h>
#include "tx_port.h"

/* threadx_event_flag_isr_set_clear_test.c */

#define ISR_ROUNDS 40

static struct k_event event_flags_0;
static struct k_timer timer_0;
static struct k_timer isr_timer;
static volatile unsigned long thread_0_counter;
static volatile unsigned long thread_1_counter;
static volatile unsigned long thread_2_counter;
static volatile unsigned long timer_0_counter;
static volatile unsigned long event_flags_set_counter;
static volatile bool isr_enabled;

static int suspended_count(void)
{
	struct k_thread *t;
	int n = 0;

	_WAIT_Q_FOR_EACH(&event_flags_0.wait_q, t) {
		n++;
	}
	return n;
}

static void test_isr(struct k_timer *timer)
{
	uint32_t actual;

	if (!isr_enabled) {
		return;
	}

	if (((k_event_test(&event_flags_0, 0x3) == 0)) && (suspended_count() == 2)) {
		k_event_post(&event_flags_0, 0x3);		/* TX_OR */
		k_event_clear(&event_flags_0, 0x00000003);	/* TX_AND 0xFFFFFFFC */
		k_event_post(&event_flags_0, 0x30000);
		k_event_clear(&event_flags_0, 0x00010000);	/* TX_AND 0xFFFEFFFF */
		k_event_clear(&event_flags_0, 0x00020003);	/* TX_AND 0xFFFDFFFC */

		if (k_event_test(&event_flags_0, 0x30000) != 0) {
			return;
		}

		k_event_post(&event_flags_0, 0x000000C0);
		/* ThreadX returns the whole group in "actual"; Zephyr returns
		 * the matched bits, so read the group first.
		 */
		actual = k_event_test(&event_flags_0, 0xFFFFFFFF);
		if ((k_event_test(&event_flags_0, 0x00000080) != 0) &&
		    (k_event_wait_safe(&event_flags_0, 0x000000C0, false,
				       K_NO_WAIT) == 0xC0) &&
		    (actual == 0xC4)) {
			event_flags_set_counter++;
		}
	}
}

static void timer_0_entry(struct k_timer *timer)
{
	timer_0_counter++;
}

static void isr_thread_0(void *p1, void *p2, void *p3)
{
	isr_enabled = true;

	/* ThreadX loops until 40 ISRs have landed inside tx_event_flags_set()
	 * with preemption disabled. Zephyr's k_event calls run with
	 * interrupts locked, so an ISR on the same CPU cannot land inside
	 * one; loop until the ISR has run its sequence 40 times instead.
	 */
	while (event_flags_set_counter < ISR_ROUNDS) {
		if (k_event_wait_safe(&event_flags_0, 2, false, TX_TICKS(4)) == 0) {
			TX_ERROR(8);
			isr_enabled = false;
			return;
		}
		thread_0_counter++;
	}

	isr_enabled = false;
	k_yield();

	if ((thread_0_counter != event_flags_set_counter) ||
	    (thread_1_counter != event_flags_set_counter)) {
		printk("thread_0_counter %lu, thread_1_counter %lu, ISR rounds %lu\n",
		       thread_0_counter, thread_1_counter, event_flags_set_counter);
		TX_ERROR(10);
	}
}

static void isr_thread_1(void *p1, void *p2, void *p3)
{
	while (true) {
		if (k_event_wait_safe(&event_flags_0, 1, false, TX_TICKS(4)) == 0) {
			break;
		}
		thread_1_counter++;
	}
}

static void isr_thread_2(void *p1, void *p2, void *p3)
{
	while (true) {
		k_event_post(&event_flags_0, 0x4);
		thread_2_counter++;
		k_yield();
	}
}

ZTEST(threadx_port, test_event_flag_isr_set_clear)
{
	/* The original creates thread 2 first, so it runs (and sets 0x4,
	 * which the ISR expects to see) before threads 0 and 1 block.
	 */
	static const struct tx_thread_def defs[] = {
		{ isr_thread_2, 17, true },
		{ isr_thread_0, 17, true },
		{ isr_thread_1, 17, true },
	};

	thread_0_counter = 0;
	thread_1_counter = 0;
	thread_2_counter = 0;
	timer_0_counter = 0;
	event_flags_set_counter = 0;
	isr_enabled = false;
	k_event_init(&event_flags_0);
	k_timer_init(&timer_0, timer_0_entry, NULL);
	k_timer_init(&isr_timer, test_isr, NULL);
	k_timer_start(&timer_0, K_TICKS(1), K_TICKS(1));
	k_timer_start(&isr_timer, K_TICKS(1), K_TICKS(1));

	tx_run_main(defs, ARRAY_SIZE(defs), 1);

	k_timer_stop(&isr_timer);
	k_timer_stop(&timer_0);
	TC_PRINT("ISR rounds %lu, thread 2 sets %lu\n", event_flags_set_counter,
		 thread_2_counter);
}

/* threadx_thread_delayed_suspension_test.c, first part */

static struct k_sem semaphore_0;
static volatile unsigned long thread_1_gets;

static void delayed_thread_0(void *p1, void *p2, void *p3)
{
	k_yield();

	/* Thread 1 is blocked on semaphore 0: suspend it, give, resume. */
	k_thread_suspend(&tx_thread[1]);
	k_sem_give(&semaphore_0);
	k_thread_resume(&tx_thread[1]);
	k_yield();

	/* Zephyr port: thread 1 took that give exactly once and blocks again. */
	if (thread_1_gets != 1) {
		TX_ERROR(100);
		return;
	}

	/* Suspend and resume it while blocked. ThreadX returns
	 * TX_SUSPEND_LIFTED: only the suspension is lifted and the thread
	 * stays blocked on the semaphore.
	 */
	k_thread_suspend(&tx_thread[1]);
	k_thread_resume(&tx_thread[1]);
	if (!tx_state(&tx_thread[1], _THREAD_PENDING) ||
	    tx_state(&tx_thread[1], _THREAD_SUSPENDED)) {
		TX_ERROR(4);
		return;
	}

	/* Zephyr port: it still takes the next give. */
	k_sem_give(&semaphore_0);
	k_yield();
	if (thread_1_gets != 2) {
		TX_ERROR(101);
	}
}

static void delayed_thread_1(void *p1, void *p2, void *p3)
{
	while (true) {
		if (k_sem_take(&semaphore_0, K_FOREVER) != 0) {
			break;
		}
		thread_1_gets++;
	}
}

ZTEST(threadx_port, test_thread_delayed_suspension)
{
	static const struct tx_thread_def defs[] = {
		{ delayed_thread_0, 2, true },
		{ delayed_thread_1, 2, true },
	};

	thread_1_gets = 0;
	k_sem_init(&semaphore_0, 0, K_SEM_MAX_LIMIT);
	tx_run(defs, ARRAY_SIZE(defs));
}
