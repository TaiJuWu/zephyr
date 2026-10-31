/*
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PORT_H_
#define PORT_H_

/* The ztest thread is cooperative by default. The ported tests assume a
 * preemptible main thread that a higher priority thread preempts as soon
 * as it is woken, so each test first moves to this priority.
 */
#define PORT_MAIN_PRIO K_PRIO_PREEMPT(10)

#endif /* PORT_H_ */
