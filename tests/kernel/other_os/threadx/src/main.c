/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tx_port.h"

struct k_thread tx_thread[TX_THREADS];
static K_THREAD_STACK_ARRAY_DEFINE(tx_stack, TX_THREADS, TX_STACK_SIZE);
volatile int tx_error;

void tx_run(const struct tx_thread_def *defs, int n)
{
	__ASSERT_NO_MSG(n <= TX_THREADS);
	tx_error = 0;

	for (int i = 0; i < n; i++) {
		k_thread_create(&tx_thread[i], tx_stack[i], TX_STACK_SIZE,
				defs[i].entry, NULL, NULL, NULL, defs[i].prio, 0,
				K_FOREVER);
	}

	/* Like the end of tx_application_define(): every auto-start thread
	 * becomes ready at once, in creation order.
	 */
	k_sched_lock();
	for (int i = 0; i < n; i++) {
		if (defs[i].auto_start) {
			k_thread_start(&tx_thread[i]);
		}
	}
	k_sched_unlock();

	int ret = k_thread_join(&tx_thread[0], K_SECONDS(30));

	for (int i = 0; i < n; i++) {
		k_thread_abort(&tx_thread[i]);
	}

	zassert_equal(ret, 0, "thread 0 did not finish");
	zassert_equal(tx_error, 0, "ThreadX ERROR #%d", tx_error);
}

ZTEST_SUITE(threadx_port, NULL, NULL, ztest_simple_1cpu_before,
	    ztest_simple_1cpu_after, NULL);
