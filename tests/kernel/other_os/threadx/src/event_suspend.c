/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ports of the ThreadX event flag suspension tests
 * (test/tx/regression/threadx_event_flag_suspension*_test.c): several
 * threads blocked on one group, woken by a single set.
 *
 * ThreadX -> Zephyr:
 * - tx_event_flags_set(TX_OR)            k_event_post()
 * - tx_event_flags_set(v, TX_AND)        k_event_clear(~v)
 * - tx_event_flags_get(TX_AND)           k_event_wait_all()
 * - tx_event_flags_get(TX_AND_CLEAR)     k_event_wait_all_safe()
 * - tx_event_flags_get(TX_OR_CLEAR)      k_event_wait_safe()
 * - TX_NO_EVENTS                         0 returned
 * tx_event_flags_get() returns the whole group in actual_events, Zephyr
 * returns the requested bits that matched, so a check of actual_events
 * against v becomes a check against (v & requested). The set notify
 * callback has no Zephyr counterpart and is left out. See README.rst.
 */

#include "tx_port.h"

static struct k_event group_0;
static volatile unsigned long thread_0_counter;
static volatile unsigned long thread_1_counter;
static volatile unsigned long thread_2_counter;
static volatile unsigned long thread_3_counter;
static volatile unsigned long thread_4_counter;

static void counters_reset(void)
{
	k_event_init(&group_0);
	thread_0_counter = 0;
	thread_1_counter = 0;
	thread_2_counter = 0;
	thread_3_counter = 0;
	thread_4_counter = 0;
}

/* threadx_event_flag_suspension_test.c */

static void susp_thread_0(void *p1, void *p2, void *p3)
{
	uint32_t event_flag = 1;

	for (int i = 0; i < 32; i++) {
		thread_0_counter++;
		k_event_post(&group_0, event_flag);

		if ((i < 31) && ((thread_1_counter != 1) || (thread_2_counter != 1) ||
				 (thread_3_counter != 1))) {
			TX_ERROR(10);
		}
		if ((i == 31) && ((thread_1_counter != 2) || (thread_2_counter != 2) ||
				  (thread_3_counter != 2))) {
			TX_ERROR(11);
		}
		event_flag <<= 1;
		if (event_flag == 0) {
			event_flag = 1;
		}
	}

	k_event_clear(&group_0, ~0U);
	k_thread_start(&tx_thread[4]);
	if (thread_4_counter != 1) {
		TX_ERROR(11);
	}

	k_event_post(&group_0, 0x1);
	if (thread_4_counter != 2) {
		TX_ERROR(12);
	}
}

static void susp_thread_1(void *p1, void *p2, void *p3)
{
	while (1) {
		thread_1_counter++;
		if (k_event_wait_all(&group_0, 0x80008000, false, K_FOREVER) != 0x80008000) {
			return;
		}
		k_event_clear(&group_0, ~0U);
	}
}

static void susp_thread_2(void *p1, void *p2, void *p3)
{
	volatile unsigned long *counter = p1;

	while (1) {
		(*counter)++;
		if (k_event_wait_all(&group_0, 0x80008000, false, K_FOREVER) != 0x80008000) {
			return;
		}
	}
}

static void susp_thread_4(void *p1, void *p2, void *p3)
{
	while (1) {
		thread_4_counter++;
		if (k_event_wait_safe(&group_0, 0x1, false, K_FOREVER) != 0x1) {
			return;
		}
	}
}

ZTEST(threadx_port, test_event_flag_suspension)
{
	const struct tx_thread_def defs[] = {
		{ susp_thread_0, 17, true },
		{ susp_thread_1, 16, true },
		{ susp_thread_2, 16, true, (void *)&thread_2_counter },
		{ susp_thread_2, 16, true, (void *)&thread_3_counter },
		{ susp_thread_4, 16, false },
	};

	counters_reset();
	tx_run(defs, ARRAY_SIZE(defs));
}

/* threadx_event_flag_suspension_consume_test.c: thread 1 consumes the bits
 * it waited for; threads 2 and 3, woken by the same set, still see them.
 */

static void consume_thread_0(void *p1, void *p2, void *p3)
{
	uint32_t event_flag = 1;

	for (int i = 0; i < 32; i++) {
		thread_0_counter++;
		k_event_post(&group_0, event_flag);

		if ((i < 31) && ((thread_1_counter != 1) || (thread_2_counter != 1) ||
				 (thread_3_counter != 1))) {
			TX_ERROR(9);
		}
		if ((i == 31) && ((thread_1_counter != 2) || (thread_2_counter != 2) ||
				  (thread_3_counter != 2))) {
			TX_ERROR(10);
		}
		event_flag <<= 1;
		if (event_flag == 0) {
			event_flag = 1;
		}
	}
}

static void consume_thread_1(void *p1, void *p2, void *p3)
{
	while (1) {
		thread_1_counter++;
		if (k_event_wait_all_safe(&group_0, 0x80008000, false, K_FOREVER) !=
		    0x80008000) {
			return;
		}
	}
}

ZTEST(threadx_port, test_event_flag_suspension_consume)
{
	const struct tx_thread_def defs[] = {
		{ consume_thread_0, 17, true },
		{ consume_thread_1, 16, true },
		{ susp_thread_2, 16, true, (void *)&thread_2_counter },
		{ susp_thread_2, 16, true, (void *)&thread_3_counter },
	};

	counters_reset();
	tx_run(defs, ARRAY_SIZE(defs));
}

/* threadx_event_flag_suspension_different_bits_test.c and
 * threadx_event_flag_suspension_different_bits_consume_test.c: two threads
 * waiting for different bits; a set wakes only the matching one.
 */

static void bits_thread_0(void *p1, void *p2, void *p3)
{
	thread_0_counter++;
	k_event_post(&group_0, 0x80000000);
	if ((thread_1_counter != 2) || (thread_2_counter != 1)) {
		TX_ERROR(7);
	}

	k_event_post(&group_0, 0x00008000);
	if ((thread_1_counter != 2) || (thread_2_counter != 2)) {
		TX_ERROR(8);
	}
}

static void bits_thread_1(void *p1, void *p2, void *p3)
{
	bool consume = POINTER_TO_INT(p1);

	while (1) {
		thread_1_counter++;
		if (consume) {
			if (k_event_wait_all_safe(&group_0, 0x80000000, false, K_FOREVER) !=
			    0x80000000) {
				return;
			}
		} else {
			if (k_event_wait_all(&group_0, 0x80000000, false, K_FOREVER) !=
			    0x80000000) {
				return;
			}
			k_event_clear(&group_0, ~0x7FFFFFFFU);
		}
	}
}

static void bits_thread_2(void *p1, void *p2, void *p3)
{
	bool consume = POINTER_TO_INT(p1);

	while (1) {
		thread_2_counter++;
		if (consume) {
			if (k_event_wait_all_safe(&group_0, 0x00008000, false, K_FOREVER) !=
			    0x00008000) {
				return;
			}
		} else {
			if (k_event_wait_all(&group_0, 0x00008000, false, K_FOREVER) !=
			    0x00008000) {
				return;
			}
			k_event_clear(&group_0, ~0xFFFF7FFFU);
		}
	}
}

ZTEST(threadx_port, test_event_flag_suspension_different_bits)
{
	const struct tx_thread_def defs[] = {
		{ bits_thread_0, 17, true },
		{ bits_thread_1, 16, true, INT_TO_POINTER(0) },
		{ bits_thread_2, 16, true, INT_TO_POINTER(0) },
	};

	counters_reset();
	tx_run(defs, ARRAY_SIZE(defs));
}

ZTEST(threadx_port, test_event_flag_suspension_different_bits_consume)
{
	const struct tx_thread_def defs[] = {
		{ bits_thread_0, 17, true },
		{ bits_thread_1, 16, true, INT_TO_POINTER(1) },
		{ bits_thread_2, 16, true, INT_TO_POINTER(1) },
	};

	counters_reset();
	tx_run(defs, ARRAY_SIZE(defs));
}

/* threadx_event_flag_suspension_timeout_test.c: threads 1 and 2 keep
 * timing out (2 and 5 ticks) while thread 0 sleeps 63 ticks. The thread
 * order is the creation order of the original: 0, 1, 3, 2.
 */

static void timeout_thread_0(void *p1, void *p2, void *p3)
{
	thread_0_counter++;
	k_event_post(&group_0, 0x00000001);
	k_thread_start(&tx_thread[2]);     /* thread 3 */
	k_sleep(K_TICKS(1));               /* thread 3 now waits on group 0 */
	k_event_post(&group_0, 0x00000002);
	k_sleep(K_TICKS(2));
	k_thread_start(&tx_thread[1]);     /* thread 1 */
	k_thread_start(&tx_thread[3]);     /* thread 2 */
	k_sleep(K_TICKS(1));

	if (k_event_wait_all_safe(&group_0, 0x00000001, false, K_FOREVER) == 0) {
		TX_ERROR(6);
	}

	k_sleep(K_TICKS(63));

	/* The original allows 32 or 33 rounds of thread 1 and 13 or 14 of
	 * thread 2, depending on the start relative to the tick boundary.
	 */
	if ((thread_1_counter < 32) || (thread_1_counter > 33) ||
	    (thread_2_counter < 13) || (thread_2_counter > 14)) {
		printk("thread_1_counter %lu, thread_2_counter %lu\n",
		       thread_1_counter, thread_2_counter);
		TX_ERROR(7);
	}
}

static void timeout_thread_1(void *p1, void *p2, void *p3)
{
	while (1) {
		thread_1_counter++;
		if (k_event_wait_all_safe(&group_0, 0x80000000, false, K_TICKS(2)) != 0) {
			return;
		}
	}
}

static void timeout_thread_2(void *p1, void *p2, void *p3)
{
	while (1) {
		thread_2_counter++;
		if (k_event_wait_all_safe(&group_0, 0x00008000, false, K_TICKS(5)) != 0) {
			return;
		}
	}
}

static void timeout_thread_3(void *p1, void *p2, void *p3)
{
	while (1) {
		thread_3_counter++;
		if (k_event_wait_all(&group_0, 0x00000002, false, K_TICKS(5)) != 0) {
			return;
		}
	}
}

ZTEST(threadx_port, test_event_flag_suspension_timeout)
{
	const struct tx_thread_def defs[] = {
		{ timeout_thread_0, 17, true },
		{ timeout_thread_1, 16, false },
		{ timeout_thread_3, 18, false },
		{ timeout_thread_2, 16, false },
	};

	counters_reset();
	tx_run(defs, ARRAY_SIZE(defs));
}
