/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared definitions for the port of the FreeRTOS standard demo tasks
 * (FreeRTOS/Demo/Common/Minimal). Each module keeps its tasks, priorities,
 * delays (in ticks; the app runs at 1000 ticks per second, as the demos do)
 * and its xAre...StillRunning()
 * check; a check thread calls every check periodically, as the demos'
 * check task does.
 */

#ifndef DEMO_H_
#define DEMO_H_

#include <zephyr/kernel.h>

/* FreeRTOS priorities count up from tskIDLE_PRIORITY (0) to
 * configMAX_PRIORITIES - 1; Zephyr's count down. The demos use
 * configMAX_PRIORITIES = 7.
 */
#define tskIDLE_PRIORITY     0
#define configMAX_PRIORITIES 7
#define FR_PRIO(p)           ((configMAX_PRIORITIES - 1) - (p))

#define FR_STACK_SIZE (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

/* Priorities and check period from Demo/Posix_GCC/main_full.c and
 * FreeRTOSConfig.h (configMAX_PRIORITIES 7, configTICK_RATE_HZ 1000,
 * configTIMER_TASK_PRIORITY configMAX_PRIORITIES - 1).
 */
#define configTIMER_TASK_PRIORITY (configMAX_PRIORITIES - 1)
#define mainCHECK_TASK_PRIORITY   (configMAX_PRIORITIES - 2)
#define mainCHECK_PERIOD_MS       10000
#define mainQUEUE_POLL_PRIORITY   (tskIDLE_PRIORITY + 1)
#define mainSEM_TEST_PRIORITY     (tskIDLE_PRIORITY + 1)
#define mainBLOCK_Q_PRIORITY      (tskIDLE_PRIORITY + 2)
#define mainCREATOR_TASK_PRIORITY (tskIDLE_PRIORITY + 3)
#define mainINTEGER_TASK_PRIORITY (tskIDLE_PRIORITY)

/* Create and start a demo task. pin0: on SMP, keep it on CPU 0 (see
 * README.rst: FreeRTOS SMP by default never runs tasks of different
 * priorities at once, Zephyr has no such mode).
 */
k_tid_t fr_task_create(k_thread_entry_t entry, const char *name, void *arg,
		       int fr_prio, bool pin0);

/* configASSERT() in the demos halts; here it records an error. */
#define FR_ASSERT(err, cond)                                                 \
	do {                                                                 \
		if (!(cond)) {                                               \
			(err) = __LINE__;                                    \
		}                                                            \
	} while (false)

static inline bool fr_is_suspended(k_tid_t t)
{
	return (t->base.thread_state & _THREAD_SUSPENDED) != 0U;
}

void vStartPolledQueueTasks(int uxPriority);
bool xArePollingQueuesStillRunning(void);
void vStartSemaphoreTasks(int uxPriority);
bool xAreSemaphoreTasksStillRunning(void);
void vStartCountingSemaphoreTasks(void);
bool xAreCountingSemaphoreTasksStillRunning(void);
void vStartBlockingQueueTasks(int uxPriority);
bool xAreBlockingQueuesStillRunning(void);
void vStartRecursiveMutexTasks(void);
bool xAreRecursiveMutexTasksStillRunning(void);
void vStartIntegerMathTasks(int uxPriority);
bool xAreIntegerMathsTaskStillRunning(void);
void vCreateSuicidalTasks(int uxPriority);
bool xIsCreateTaskStillRunning(void);
void vStartDynamicPriorityTasks(void);
bool xAreDynamicPriorityTasksStillRunning(void);
void vCreateBlockTimeTasks(void);
bool xAreBlockTimeTestTasksStillRunning(void);
void vStartEventGroupTasks(void);
bool xAreEventGroupTasksStillRunning(void);

#endif /* DEMO_H_ */
