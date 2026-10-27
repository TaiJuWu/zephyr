/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FreeRTOS full demo as a Zephyr soak test: start the demo modules in
 * the order of Demo/Posix_GCC/main_full.c, then, like its check task, call
 * every module's check each mainCHECK_PERIOD_MS. Any failed check fails the
 * run; the run lasts CONFIG_FREERTOS_DEMO_DURATION_S.
 */

#include <zephyr/ztest.h>
#include "demo.h"

#define MAX_TASKS 32

static K_THREAD_STACK_ARRAY_DEFINE(task_stack, MAX_TASKS, FR_STACK_SIZE);
static struct k_thread task_thread[MAX_TASKS];
static int n_tasks;

k_tid_t fr_task_create(k_thread_entry_t entry, const char *name, void *arg,
		       int fr_prio, bool pin0)
{
	__ASSERT(n_tasks < MAX_TASKS, "too many demo tasks");
	struct k_thread *t = &task_thread[n_tasks];

	k_thread_create(t, task_stack[n_tasks], FR_STACK_SIZE, entry, arg, NULL, NULL,
			FR_PRIO(fr_prio), 0, K_FOREVER);
	n_tasks++;
	k_thread_name_set(t, name);
#ifdef CONFIG_SCHED_CPU_MASK
	if (pin0) {
		k_thread_cpu_pin(t, 0);
	}
#endif
	k_thread_start(t);
	return t;
}

struct check {
	const char *name;
	bool (*fn)(void);
	int module;
};

enum { M_BLOCKQ, M_SEMTEST, M_POLLQ, M_INTEGER, M_RECMUTEX, M_COUNTSEM, M_DYNAMIC,
       M_BLOCKTIM, M_DEATH };

#define ON(m) ((CONFIG_FREERTOS_DEMO_MODULES & BIT(m)) != 0)

static const struct check checks[] = {
	{ "xIsCreateTaskStillRunning", xIsCreateTaskStillRunning, M_DEATH },
	{ "xAreBlockTimeTestTasksStillRunning", xAreBlockTimeTestTasksStillRunning, M_BLOCKTIM },
	{ "xAreSemaphoreTasksStillRunning", xAreSemaphoreTasksStillRunning, M_SEMTEST },
	{ "xArePollingQueuesStillRunning", xArePollingQueuesStillRunning, M_POLLQ },
	{ "xAreIntegerMathsTaskStillRunning", xAreIntegerMathsTaskStillRunning, M_INTEGER },
	{ "xAreRecursiveMutexTasksStillRunning", xAreRecursiveMutexTasksStillRunning, M_RECMUTEX },
	{ "xAreBlockingQueuesStillRunning", xAreBlockingQueuesStillRunning, M_BLOCKQ },
	{ "xAreCountingSemaphoreTasksStillRunning", xAreCountingSemaphoreTasksStillRunning,
	  M_COUNTSEM },
	{ "xAreDynamicPriorityTasksStillRunning", xAreDynamicPriorityTasksStillRunning,
	  M_DYNAMIC },
};

ZTEST(freertos_demo, test_full_demo)
{
	int64_t end = k_uptime_get() + CONFIG_FREERTOS_DEMO_DURATION_S * MSEC_PER_SEC;
	int64_t next = k_uptime_get();
	int failures = 0, rounds = 0;

	/* The check task runs at mainCHECK_TASK_PRIORITY. */
	k_thread_priority_set(k_current_get(), FR_PRIO(mainCHECK_TASK_PRIORITY));

	if (ON(M_BLOCKQ)) {
		vStartBlockingQueueTasks(mainBLOCK_Q_PRIORITY);
	}
	if (ON(M_SEMTEST)) {
		vStartSemaphoreTasks(mainSEM_TEST_PRIORITY);
	}
	if (ON(M_POLLQ)) {
		vStartPolledQueueTasks(mainQUEUE_POLL_PRIORITY);
	}
	if (ON(M_INTEGER)) {
		vStartIntegerMathTasks(mainINTEGER_TASK_PRIORITY);
	}
	if (ON(M_RECMUTEX)) {
		vStartRecursiveMutexTasks();
	}
	if (ON(M_COUNTSEM)) {
		vStartCountingSemaphoreTasks();
	}
	if (ON(M_DYNAMIC)) {
		vStartDynamicPriorityTasks();
	}
	if (ON(M_BLOCKTIM)) {
		vCreateBlockTimeTasks();
	}
	/* Last, so the task count it records at start includes the others. */
	if (ON(M_DEATH)) {
		vCreateSuicidalTasks(mainCREATOR_TASK_PRIORITY);
	}

	while (k_uptime_get() < end) {
		next += mainCHECK_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
		rounds++;

		const char *pcMessage = "PASS";

		for (size_t i = 0; i < ARRAY_SIZE(checks); i++) {
			if (!ON(checks[i].module)) {
				continue;
			}
			if (!checks[i].fn()) {
				pcMessage = checks[i].name;
				failures++;
				printk("check %d: %s() returned false\n", rounds, checks[i].name);
			}
		}
		printk("check %d at %lld s: %s\n", rounds, k_uptime_get() / MSEC_PER_SEC,
		       pcMessage);
	}

	for (int i = 0; i < n_tasks; i++) {
		k_thread_abort(&task_thread[i]);
	}
	zassert_equal(failures, 0, "%d failed checks in %d rounds", failures, rounds);
}

ZTEST_SUITE(freertos_demo, NULL, NULL, NULL, NULL, NULL);
