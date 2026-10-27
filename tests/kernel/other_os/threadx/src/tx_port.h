/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Helpers shared by the ports of the ThreadX regression tests
 * (eclipse-threadx/threadx, test/tx/regression).
 *
 * A ThreadX test creates its threads in tx_application_define(), before the
 * scheduler runs, and reports through test_control_return(). Here the ztest
 * thread creates the same threads with the same priorities (ThreadX and
 * Zephyr both run lower numbers first), starts them in creation order with
 * the scheduler locked, and then waits for thread 0. The first ThreadX error
 * number hit is kept and checked by the ztest thread.
 */

#ifndef TX_PORT_H_
#define TX_PORT_H_

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define TX_THREADS     3
#define TX_STACK_SIZE  (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

extern struct k_thread tx_thread[TX_THREADS];
extern volatile int tx_error;

/* Record the first failing ThreadX error number. */
#define TX_ERROR(n)                                                          \
	do {                                                                 \
		if (tx_error == 0) {                                         \
			tx_error = (n);                                      \
		}                                                            \
	} while (false)

struct tx_thread_def {
	k_thread_entry_t entry;
	int prio;
	bool auto_start;
};

/* Create the threads as tx_application_define() would and run the test
 * until thread 0 returns. Fails the test on a recorded error or a hang.
 */
void tx_run(const struct tx_thread_def *defs, int n);

static inline bool tx_state(struct k_thread *t, uint8_t flag)
{
	return (t->base.thread_state & flag) != 0U;
}

#endif /* TX_PORT_H_ */
