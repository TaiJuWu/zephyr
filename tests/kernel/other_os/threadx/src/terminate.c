/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ports of the ThreadX "thread terminate while suspended on an object" tests:
 * a thread blocked on a kernel object is terminated (k_thread_abort()), and
 * the object must be left clean. tx_thread_terminate() maps to
 * k_thread_abort(), tx_thread_relinquish() to k_yield(), and ThreadX ticks to
 * K_TICKS(). Checks marked "Zephyr port" are not in the original; see
 * README.rst.
 */

#include <wait_q.h>
#include "tx_port.h"

static volatile unsigned long thread_0_counter;
static volatile unsigned long thread_1_counter;
static volatile unsigned long thread_2_counter;

static void reset_counters(void)
{
	thread_0_counter = 0;
	thread_1_counter = 0;
	thread_2_counter = 0;
}

/* Zephyr port: the terminated waiters left the wait queue and their timeouts
 * do not fire after the abort.
 */
static void check_waitq_empty(_wait_q_t *wq, int err)
{
	if (z_waitq_head(wq) != NULL) {
		TX_ERROR(err);
	}
}

/* threadx_semaphore_thread_terminate_test.c */

static struct k_sem semaphore_0;

static void sem_thread_0(void *p1, void *p2, void *p3)
{
	thread_0_counter++;
	k_yield();

	if ((thread_1_counter != 1) || (thread_2_counter != 1)) {
		TX_ERROR(7);
		return;
	}

	k_thread_abort(&tx_thread[1]);
	if ((thread_1_counter != 1) || (thread_2_counter != 1)) {
		TX_ERROR(8);
		return;
	}

	k_thread_abort(&tx_thread[2]);
	k_yield();
	if ((thread_1_counter != 1) || (thread_2_counter != 1)) {
		TX_ERROR(9);
		return;
	}

	/* Zephyr port: past both timeouts, the waiters stay gone and the
	 * semaphore still counts.
	 */
	check_waitq_empty(&semaphore_0.wait_q, 100);
	k_sleep(K_TICKS(50));
	if ((thread_1_counter != 1) || (thread_2_counter != 1)) {
		TX_ERROR(101);
	}
	k_sem_give(&semaphore_0);
	if (k_sem_take(&semaphore_0, K_NO_WAIT) != 0) {
		TX_ERROR(102);
	}
}

static void sem_thread_1(void *p1, void *p2, void *p3)
{
	thread_1_counter++;
	k_sem_take(&semaphore_0, K_TICKS(33));
	thread_1_counter++;
}

static void sem_thread_2(void *p1, void *p2, void *p3)
{
	thread_2_counter++;
	k_sem_take(&semaphore_0, K_TICKS(44));
	thread_2_counter++;
}

ZTEST(threadx_port, test_semaphore_thread_terminate)
{
	static const struct tx_thread_def defs[] = {
		{ sem_thread_0, 16, true },
		{ sem_thread_1, 16, true },
		{ sem_thread_2, 16, true },
	};

	reset_counters();
	k_sem_init(&semaphore_0, 0, K_SEM_MAX_LIMIT);
	tx_run(defs, ARRAY_SIZE(defs));
}

/* threadx_mutex_thread_terminate_test.c */

static struct k_mutex mutex_0;

static void mutex_thread_0(void *p1, void *p2, void *p3)
{
	thread_0_counter++;
	k_yield();

	if ((thread_1_counter != 1) || (thread_2_counter != 1)) {
		TX_ERROR(7);
		return;
	}

	k_thread_abort(&tx_thread[1]);
	if ((thread_1_counter != 1) || (thread_2_counter != 1)) {
		TX_ERROR(8);
		return;
	}

	k_thread_abort(&tx_thread[2]);
	k_yield();
	if ((thread_1_counter != 1) || (thread_2_counter != 1)) {
		TX_ERROR(9);
		return;
	}

	check_waitq_empty(&mutex_0.wait_q, 100);
	k_sleep(K_TICKS(50));
	if ((thread_1_counter != 1) || (thread_2_counter != 1)) {
		TX_ERROR(101);
	}
}

static void mutex_thread_1(void *p1, void *p2, void *p3)
{
	thread_1_counter++;
	k_mutex_lock(&mutex_0, K_TICKS(33));
	thread_1_counter++;
}

static void mutex_thread_2(void *p1, void *p2, void *p3)
{
	thread_2_counter++;
	k_mutex_lock(&mutex_0, K_TICKS(44));
	thread_2_counter++;
}

ZTEST(threadx_port, test_mutex_thread_terminate)
{
	static const struct tx_thread_def defs[] = {
		{ mutex_thread_0, 16, true },
		{ mutex_thread_1, 16, true },
		{ mutex_thread_2, 16, true },
	};
	int prio = k_thread_priority_get(k_current_get());

	reset_counters();
	k_mutex_init(&mutex_0);
	/* ThreadX takes the mutex in tx_application_define(); the ztest
	 * thread is the owner here.
	 */
	k_mutex_lock(&mutex_0, K_NO_WAIT);
	tx_run(defs, ARRAY_SIZE(defs));

	/* Zephyr port: the owner can still release it and was not left with
	 * a boost from the dead waiters.
	 */
	zassert_equal(k_thread_priority_get(k_current_get()), prio);
	zassert_equal(k_mutex_unlock(&mutex_0), 0);
	zassert_equal(k_mutex_lock(&mutex_0, K_NO_WAIT), 0);
	zassert_equal(k_mutex_unlock(&mutex_0), 0);
}

/* threadx_queue_thread_terminate_test.c */

static struct k_msgq queue_0;
static char __aligned(4) queue_0_buf[3 * 2 * sizeof(uint32_t)];

static void queue_thread_0(void *p1, void *p2, void *p3)
{
	thread_0_counter++;

	if (!tx_state(&tx_thread[1], _THREAD_PENDING)) {
		TX_ERROR(5);
		return;
	}

	k_thread_abort(&tx_thread[1]);
	if (!tx_state(&tx_thread[1], _THREAD_DEAD) || (thread_1_counter != 0) ||
	    (z_waitq_head(&queue_0.wait_q) != NULL)) {
		TX_ERROR(6);
		return;
	}

	/* Zephyr port: a message sent now stays in the queue. */
	uint32_t msg[2] = { 0x12345678, 0 };
	uint32_t out[2];

	if ((k_msgq_put(&queue_0, msg, K_NO_WAIT) != 0) ||
	    (k_msgq_num_used_get(&queue_0) != 1) ||
	    (k_msgq_get(&queue_0, out, K_NO_WAIT) != 0) || (out[0] != msg[0])) {
		TX_ERROR(100);
	}
}

static void queue_thread_1(void *p1, void *p2, void *p3)
{
	uint32_t dest_message[2];

	while (true) {
		if (k_msgq_get(&queue_0, dest_message, K_FOREVER) != -ENOMSG) {
			break;
		}
		thread_1_counter++;
	}
}

ZTEST(threadx_port, test_queue_thread_terminate)
{
	static const struct tx_thread_def defs[] = {
		{ queue_thread_0, 17, true },
		{ queue_thread_1, 16, true },
	};

	reset_counters();
	k_msgq_init(&queue_0, queue_0_buf, 2 * sizeof(uint32_t), 3);
	tx_run(defs, ARRAY_SIZE(defs));
}

/* threadx_block_memory_thread_terminate_test.c */

/* ThreadX uses 100-byte blocks; Zephyr needs a multiple of the pointer size. */
#define BLOCK_SIZE ROUND_UP(100, sizeof(void *))
static struct k_mem_slab pool_0;
static char __aligned(8) pool_0_buf[3 * BLOCK_SIZE];

static void block_thread_0(void *p1, void *p2, void *p3)
{
	void *pointer_1;
	void *pointer_2;
	void *pointer_3;
	int status;

	status = k_mem_slab_alloc(&pool_0, &pointer_1, K_NO_WAIT);
	status |= k_mem_slab_alloc(&pool_0, &pointer_2, K_NO_WAIT);
	status |= k_mem_slab_alloc(&pool_0, &pointer_3, K_NO_WAIT);
	thread_0_counter++;
	if (status != 0) {
		TX_ERROR(5);
		return;
	}

	memset(pointer_1, 0xEF, BLOCK_SIZE);
	memset(pointer_2, 0xEF, BLOCK_SIZE);
	memset(pointer_3, 0xEF, BLOCK_SIZE);

	k_yield();

	k_thread_abort(&tx_thread[1]);
	if (!tx_state(&tx_thread[1], _THREAD_DEAD) || (thread_1_counter != 0)) {
		TX_ERROR(6);
		return;
	}

	k_mem_slab_free(&pool_0, pointer_1);
	k_mem_slab_free(&pool_0, pointer_2);
	k_mem_slab_free(&pool_0, pointer_3);
	if (!tx_state(&tx_thread[1], _THREAD_DEAD) || (thread_1_counter != 0)) {
		TX_ERROR(7);
		return;
	}

	/* Zephyr port: no block went to the dead waiter. */
	if (k_mem_slab_num_free_get(&pool_0) != 3) {
		TX_ERROR(100);
	}
}

static void block_thread_1(void *p1, void *p2, void *p3)
{
	void *pointer_1;

	while (true) {
		if (k_mem_slab_alloc(&pool_0, &pointer_1, K_TICKS(10)) != -EAGAIN) {
			return;
		}
		thread_1_counter++;
	}
}

ZTEST(threadx_port, test_block_memory_thread_terminate)
{
	static const struct tx_thread_def defs[] = {
		{ block_thread_0, 17, true },
		{ block_thread_1, 17, true },
	};

	reset_counters();
	zassert_ok(k_mem_slab_init(&pool_0, pool_0_buf, BLOCK_SIZE, 3));
	tx_run(defs, ARRAY_SIZE(defs));
}

/* threadx_byte_memory_thread_terminate_test.c */

#define BYTE_ALLOC 60
static struct k_heap heap_0;
static char __aligned(8) heap_0_buf[512];

/* ThreadX sizes the pool (108 bytes) so that one 60-byte block fits and a
 * second does not. Zephyr's heap overhead differs, so find that size here.
 */
static void heap_0_init_one_block(void)
{
	for (size_t sz = BYTE_ALLOC; sz <= sizeof(heap_0_buf); sz += 4) {
		k_heap_init(&heap_0, heap_0_buf, sz);
		void *p = k_heap_alloc(&heap_0, BYTE_ALLOC, K_NO_WAIT);

		if (p != NULL) {
			void *q = k_heap_alloc(&heap_0, BYTE_ALLOC, K_NO_WAIT);

			zassert_is_null(q, "two blocks fit in %zu bytes", sz);
			k_heap_free(&heap_0, p);
			return;
		}
	}
	zassert_unreachable("no heap size fits one block");
}

static void byte_thread_0(void *p1, void *p2, void *p3)
{
	void *pointer;

	thread_0_counter++;
	pointer = k_heap_alloc(&heap_0, BYTE_ALLOC, K_NO_WAIT);
	if (pointer == NULL) {
		TX_ERROR(5);
		return;
	}

	k_yield();

	k_thread_abort(&tx_thread[1]);
	k_heap_free(&heap_0, pointer);

	pointer = k_heap_alloc(&heap_0, BYTE_ALLOC, K_NO_WAIT);
	if ((pointer == NULL) || (thread_1_counter != 0)) {
		TX_ERROR(6);
		return;
	}
	k_heap_free(&heap_0, pointer);
}

static void byte_thread_1(void *p1, void *p2, void *p3)
{
	while (true) {
		if (k_heap_alloc(&heap_0, BYTE_ALLOC, K_FOREVER) != NULL) {
			return;
		}
		thread_1_counter++;
	}
}

ZTEST(threadx_port, test_byte_memory_thread_terminate)
{
	static const struct tx_thread_def defs[] = {
		{ byte_thread_0, 17, true },
		{ byte_thread_1, 17, true },
	};

	reset_counters();
	heap_0_init_one_block();
	tx_run(defs, ARRAY_SIZE(defs));
}

/* threadx_event_flag_thread_terminate_test.c */

static struct k_event group_0;

static void event_thread_0(void *p1, void *p2, void *p3)
{
	thread_0_counter++;

	k_thread_abort(&tx_thread[2]);
	k_thread_abort(&tx_thread[1]);
	k_event_post(&group_0, 0xFFFFFFFF);

	if ((thread_1_counter != 1) || (thread_2_counter != 1)) {
		TX_ERROR(7);
		return;
	}

	/* Zephyr port: the dead waiters consumed nothing. */
	if (k_event_test(&group_0, 0xFFFFFFFF) != 0xFFFFFFFF) {
		TX_ERROR(100);
	}
	check_waitq_empty(&group_0.wait_q, 101);
}

static void event_thread_1(void *p1, void *p2, void *p3)
{
	while (true) {
		thread_1_counter++;
		/* TX_AND_CLEAR */
		if (k_event_wait_all_safe(&group_0, 0x80000000, false,
					  K_FOREVER) == 0) {
			return;
		}
	}
}

static void event_thread_2(void *p1, void *p2, void *p3)
{
	while (true) {
		thread_2_counter++;
		if (k_event_wait_all_safe(&group_0, 0x00008000, false,
					  K_FOREVER) == 0) {
			return;
		}
	}
}

ZTEST(threadx_port, test_event_flag_thread_terminate)
{
	static const struct tx_thread_def defs[] = {
		{ event_thread_0, 17, true },
		{ event_thread_1, 16, true },
		{ event_thread_2, 16, true },
	};

	reset_counters();
	k_event_init(&group_0);
	tx_run(defs, ARRAY_SIZE(defs));
}

/* threadx_event_flag_single_thread_terminate_test.c
 *
 * The original's middle checks use "&&" and so can only fail on a bad
 * status; they are kept as written. The last checks use "||".
 */

static void single_thread_0(void *p1, void *p2, void *p3)
{
	thread_0_counter++;
	k_sleep(K_TICKS(5));

	k_thread_start(&tx_thread[2]);	/* tx_thread_resume() of TX_DONT_START */
	k_thread_abort(&tx_thread[1]);	/* thread 1 has suspended itself */
	k_sleep(K_TICKS(5));

	k_event_post(&group_0, 0x00080000);
	k_sleep(K_TICKS(5));

	k_event_post(&group_0, 0x00800000);
	k_sleep(K_TICKS(5));

	/* Thread 2 waits again; set a flag it does not need. */
	k_event_post(&group_0, 0x00000001);
	k_sleep(K_TICKS(5));

	k_event_post(&group_0, 0x00080001);
	k_sleep(K_TICKS(5));

	if ((thread_1_counter != 1) || (thread_2_counter != 3)) {
		TX_ERROR(12);
		return;
	}

	k_thread_abort(&tx_thread[2]);
	if (thread_2_counter != 3) {
		TX_ERROR(13);
	}
}

static void single_thread_1(void *p1, void *p2, void *p3)
{
	while (true) {
		thread_1_counter++;
		k_thread_suspend(k_current_get());
	}
}

static void single_thread_2(void *p1, void *p2, void *p3)
{
	while (true) {
		thread_2_counter++;
		/* TX_AND_CLEAR */
		if (k_event_wait_all_safe(&group_0, 0x00880000, false,
					  K_FOREVER) == 0) {
			return;
		}
		thread_2_counter++;
		/* TX_OR_CLEAR */
		if (k_event_wait_safe(&group_0, 0x00880000, false,
				      K_FOREVER) == 0) {
			return;
		}
		thread_2_counter++;
		/* The original expects TX_NO_EVENTS here and returns on
		 * anything else; it is never satisfied in the test.
		 */
		if (k_event_wait_all_safe(&group_0, 0x00880000, false,
					  K_FOREVER) != 0) {
			return;
		}
	}
}

ZTEST(threadx_port, test_event_flag_single_thread_terminate)
{
	static const struct tx_thread_def defs[] = {
		{ single_thread_0, 17, true },
		{ single_thread_1, 18, true },
		{ single_thread_2, 18, false },
	};

	reset_counters();
	k_event_init(&group_0);
	tx_run(defs, ARRAY_SIZE(defs));
}

/* threadx_thread_sleep_terminate_test.c */

static void sleep_thread_0(void *p1, void *p2, void *p3)
{
	thread_0_counter++;
	k_sleep(K_TICKS(5));

	k_thread_abort(&tx_thread[1]);
	if (thread_1_counter != 1) {
		TX_ERROR(3);
		return;
	}

	/* ThreadX: tx_thread_suspend() of a terminated thread returns
	 * TX_SUSPEND_ERROR and does nothing. k_thread_suspend() has no return
	 * value (and does set the suspended flag on a dead thread), so check
	 * that the thread stays dead and a resume does not run it again.
	 */
	k_thread_suspend(&tx_thread[1]);
	k_thread_resume(&tx_thread[1]);
	k_yield();
	if (!tx_state(&tx_thread[1], _THREAD_DEAD) || (thread_1_counter != 1)) {
		TX_ERROR(4);
		return;
	}

	/* Zephyr port: past the 100-tick sleep, it never ran again. */
	k_sleep(K_TICKS(110));
	if (thread_1_counter != 1) {
		TX_ERROR(100);
	}
}

static void sleep_thread_1(void *p1, void *p2, void *p3)
{
	while (true) {
		thread_1_counter++;
		k_sleep(K_TICKS(100));
	}
}

ZTEST(threadx_port, test_thread_sleep_terminate)
{
	static const struct tx_thread_def defs[] = {
		{ sleep_thread_0, 17, true },
		{ sleep_thread_1, 18, true },
	};

	reset_counters();
	tx_run(defs, ARRAY_SIZE(defs));
}
