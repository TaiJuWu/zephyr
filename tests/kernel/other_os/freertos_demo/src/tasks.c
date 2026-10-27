/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * death.c, dynamic.c and blocktim.c. vTaskSuspendAll() becomes
 * k_sched_lock(), vTaskDelete() k_thread_abort(), xTaskGetTickCount()
 * the low 32 bits of k_uptime_ticks().
 */

#include "demo.h"

static uint32_t xTaskGetTickCount(void)
{
	return (uint32_t)k_uptime_ticks();
}

/* uxTaskGetNumberOfTasks() */
static void count_cb(const struct k_thread *t, void *user_data)
{
	(*(unsigned int *)user_data)++;
}

static unsigned int uxTaskGetNumberOfTasks(void)
{
	unsigned int n = 0;

	k_thread_foreach(count_cb, &n);
	return n;
}

/* ---- death.c ---- */

#define SUICIDE_SLOTS 6

static K_THREAD_STACK_ARRAY_DEFINE(suicide_stack, SUICIDE_SLOTS, FR_STACK_SIZE);
static struct k_thread suicide_thread[SUICIDE_SLOTS];
static bool suicide_used[SUICIDE_SLOTS];
static volatile uint16_t usCreationCount;
static volatile unsigned int uxTasksRunningAtStart;
static const unsigned int uxMaxNumberOfExtraTasksRunning = 3;
static k_tid_t xCreatedTask;

static void vSuicidalTask(void *p1, void *p2, void *p3)
{
	volatile long l1, l2;
	k_tid_t xTaskToKill = (p1 != NULL) ? *(k_tid_t *)p1 : NULL;

	for (;;) {
		l1 = 2;
		l2 = 89;
		l2 *= l1;
		k_sleep(K_TICKS(200));
		if (xTaskToKill != NULL) {
			k_yield();	/* vTaskDelay(0) */
			k_thread_abort(xTaskToKill);
			k_thread_abort(k_current_get());
		}
	}
}

/* xTaskCreate() from a pool of thread objects; a slot is free again once
 * its thread is dead.
 */
static k_tid_t suicide_create(void *arg, int prio)
{
	for (int i = 0; i < SUICIDE_SLOTS; i++) {
		if (suicide_used[i] && (k_thread_join(&suicide_thread[i], K_NO_WAIT) != 0)) {
			continue;
		}
		suicide_used[i] = true;
		return k_thread_create(&suicide_thread[i], suicide_stack[i], FR_STACK_SIZE,
				       vSuicidalTask, arg, NULL, NULL, prio, 0, K_NO_WAIT);
	}
	return NULL;
}

static void vCreateTasks(void *p1, void *p2, void *p3)
{
	int prio = k_thread_priority_get(k_current_get());

	k_sleep(K_TICKS(1000));
	uxTasksRunningAtStart = uxTaskGetNumberOfTasks();

	for (;;) {
		k_sleep(K_TICKS(1000));
		xCreatedTask = NULL;
		xCreatedTask = suicide_create(NULL, prio);
		suicide_create(&xCreatedTask, prio);
		++usCreationCount;
	}
}

void vCreateSuicidalTasks(int uxPriority)
{
	fr_task_create(vCreateTasks, "CREATOR", NULL, uxPriority, false);
}

bool xIsCreateTaskStillRunning(void)
{
	static uint16_t usLastCreationCount = 0xfff;
	unsigned int uxTasksRunningNow;
	bool xReturn = true;

	if (usLastCreationCount == usCreationCount) {
		xReturn = false;
	} else {
		usLastCreationCount = usCreationCount;
	}
	uxTasksRunningNow = uxTaskGetNumberOfTasks();
	if (uxTasksRunningNow < uxTasksRunningAtStart) {
		xReturn = false;
	} else if ((uxTasksRunningNow - uxTasksRunningAtStart) > uxMaxNumberOfExtraTasksRunning) {
		xReturn = false;
	}
	return xReturn;
}

/* ---- dynamic.c ---- */

#define priSLEEP_TIME             128
#define priLOOPS                  5
#define priMAX_COUNT              0xffU
#define priSUSPENDED_QUEUE_LENGTH 1

static k_tid_t xContinuousIncrementHandle, xLimitedIncrementHandle;
static uint32_t ulCounter;
static volatile uint16_t usCheckVariable;
static volatile bool xSuspendedQueueSendError;
static volatile bool xSuspendedQueueReceiveError;
static volatile int xDynError;
static struct k_msgq xSuspendedTestQueue;
static char __aligned(4) susp_buf[priSUSPENDED_QUEUE_LENGTH * sizeof(uint32_t)];
static uint32_t ulExpectedValue;

static void vLimitedIncrementTask(void *p1, void *p2, void *p3)
{
	volatile uint32_t *pulCounter = p1;

	k_thread_suspend(k_current_get());
	for (;;) {
		(*pulCounter)++;
		if (*pulCounter >= priMAX_COUNT) {
			k_thread_suspend(k_current_get());
		}
	}
}

static void vContinuousIncrementTask(void *p1, void *p2, void *p3)
{
	volatile uint32_t *pulCounter = p1;
	int uxOurPriority = k_thread_priority_get(k_current_get());

	for (;;) {
		/* vTaskPrioritySet(NULL, uxOurPriority + 1): one step up */
		k_thread_priority_set(k_current_get(), uxOurPriority - 1);
		FR_ASSERT(xDynError, k_thread_priority_get(k_current_get()) == uxOurPriority - 1);
		(*pulCounter)++;
		k_thread_priority_set(k_current_get(), uxOurPriority);
		FR_ASSERT(xDynError, k_thread_priority_get(k_current_get()) == uxOurPriority);
	}
}

static void vCounterControlTask(void *p1, void *p2, void *p3)
{
	uint32_t ulLastCounter;
	bool sError = false;

	for (;;) {
		ulCounter = 0;
		for (int sLoops = 0; sLoops < priLOOPS; sLoops++) {
			k_thread_suspend(xContinuousIncrementHandle);
			FR_ASSERT(xDynError, fr_is_suspended(xContinuousIncrementHandle));
			ulLastCounter = ulCounter;
			k_thread_resume(xContinuousIncrementHandle);
			FR_ASSERT(xDynError, !fr_is_suspended(xContinuousIncrementHandle));
			k_sleep(K_TICKS(priSLEEP_TIME));
			k_sched_lock();
			if (ulLastCounter == ulCounter) {
				sError = true;
			}
			k_sched_unlock();
		}

		k_thread_suspend(xContinuousIncrementHandle);
		ulCounter = 0;
		FR_ASSERT(xDynError, fr_is_suspended(xLimitedIncrementHandle));
		k_thread_resume(xLimitedIncrementHandle);
		/* It has a higher priority, so it has run to priMAX_COUNT and
		 * suspended itself again.
		 */
		FR_ASSERT(xDynError, fr_is_suspended(xLimitedIncrementHandle));
		if (ulCounter != priMAX_COUNT) {
			sError = true;
		}
		if (!sError) {
			unsigned int key = irq_lock();

			usCheckVariable++;
			irq_unlock(key);
		}
		k_thread_resume(xContinuousIncrementHandle);
	}
}

static void vQueueSendWhenSuspendedTask(void *p1, void *p2, void *p3)
{
	static uint32_t ulValueToSend;

	for (;;) {
		k_sched_lock();
		if (k_msgq_put(&xSuspendedTestQueue, &ulValueToSend, K_NO_WAIT) != 0) {
			xSuspendedQueueSendError = true;
		}
		k_sched_unlock();
		k_sleep(K_TICKS(priSLEEP_TIME));
		++ulValueToSend;
	}
}

static void vQueueReceiveWhenSuspendedTask(void *p1, void *p2, void *p3)
{
	uint32_t ulReceivedValue;
	bool xGotValue;

	for (;;) {
		do {
			/* Nested suspension. The original also checks that the
			 * inner xTaskResumeAll() reports no yield;
			 * k_sched_unlock() returns nothing.
			 */
			k_sched_lock();
			k_sched_lock();
			xGotValue = k_msgq_get(&xSuspendedTestQueue, &ulReceivedValue,
					       K_NO_WAIT) == 0;
			k_sched_unlock();
			k_sched_unlock();
		} while (!xGotValue);

		if (ulReceivedValue != ulExpectedValue) {
			xSuspendedQueueReceiveError = true;
		}
		if (!xSuspendedQueueReceiveError) {
			++ulExpectedValue;
		}
	}
}

void vStartDynamicPriorityTasks(void)
{
	k_msgq_init(&xSuspendedTestQueue, susp_buf, sizeof(uint32_t), priSUSPENDED_QUEUE_LENGTH);
	xContinuousIncrementHandle = fr_task_create(vContinuousIncrementTask, "CNT_INC",
						    &ulCounter, tskIDLE_PRIORITY, true);
	xLimitedIncrementHandle = fr_task_create(vLimitedIncrementTask, "LIM_INC", &ulCounter,
						 tskIDLE_PRIORITY + 1, true);
	fr_task_create(vCounterControlTask, "C_CTRL", NULL, tskIDLE_PRIORITY, true);
	fr_task_create(vQueueSendWhenSuspendedTask, "SUSP_TX", NULL, tskIDLE_PRIORITY, true);
	fr_task_create(vQueueReceiveWhenSuspendedTask, "SUSP_RX", NULL, tskIDLE_PRIORITY, true);
}

bool xAreDynamicPriorityTasksStillRunning(void)
{
	static uint16_t usLastTaskCheck;
	static uint32_t ulLastExpectedValue;
	bool xReturn = true;

	if (usCheckVariable == usLastTaskCheck) {
		xReturn = false;
	}
	if (ulExpectedValue == ulLastExpectedValue) {
		xReturn = false;
	}
	if (xSuspendedQueueSendError || xSuspendedQueueReceiveError || (xDynError != 0)) {
		xReturn = false;
	}
	if (xDynError != 0) {
		printk("dynamic error at line %d\n", xDynError);
	}
	usLastTaskCheck = usCheckVariable;
	ulLastExpectedValue = ulExpectedValue;
	return xReturn;
}

/* ---- blocktim.c ---- */

#define bktPRIMARY_PRIORITY   (configMAX_PRIORITIES - 3)
#define bktSECONDARY_PRIORITY (configMAX_PRIORITIES - 4)
#define bktQUEUE_LENGTH       5
#define bktSHORT_WAIT         20
#define bktPRIMARY_BLOCK_TIME 10
#define bktALLOWABLE_MARGIN   15
#define bktTIME_TO_BLOCK      175
#define bktRUN_INDICATOR      0x55U

static struct k_msgq xTestQueue;
static char __aligned(4) bkt_buf[bktQUEUE_LENGTH * sizeof(int)];
static k_tid_t xSecondary;
static volatile int xPrimaryCycles, xSecondaryCycles;
static volatile int xBktError;
static volatile unsigned int xRunIndicator;

/* xTaskDelayUntil(): wake at *last + period if that is still ahead and
 * report whether it blocked; *last moves on by one period either way.
 */
static bool xTaskDelayUntil(uint32_t *pxPreviousWakeTime, uint32_t xTimeIncrement)
{
	uint32_t now = xTaskGetTickCount();
	uint32_t next = *pxPreviousWakeTime + xTimeIncrement;
	bool xShouldDelay;

	if (now < *pxPreviousWakeTime) {
		/* The tick count wrapped since the last call. */
		xShouldDelay = (next < *pxPreviousWakeTime) && (next > now);
	} else {
		xShouldDelay = (next < *pxPreviousWakeTime) || (next > now);
	}
	*pxPreviousWakeTime = next;
	if (xShouldDelay) {
		k_sleep(K_TIMEOUT_ABS_TICKS(k_uptime_ticks() + (int32_t)(next - now)));
	}
	return xShouldDelay;
}

static void check_blocked(uint32_t xTimeWhenBlocking, uint32_t xTimeToBlock, int line)
{
	uint32_t xBlockedTime = xTaskGetTickCount() - xTimeWhenBlocking;

	if ((xBlockedTime < xTimeToBlock) ||
	    (xBlockedTime > (xTimeToBlock + bktALLOWABLE_MARGIN))) {
		xBktError = line;
	}
}

static void prvBasicDelayTests(void)
{
	uint32_t xPreTime, xPostTime, xLastUnblockTime, xExpectedUnblockTime;
	const uint32_t xPeriod = 75, xCycles = 5, xAllowableMargin = bktALLOWABLE_MARGIN >> 1;
	const uint32_t xHalfPeriod = xPeriod / 2;
	bool xDidBlock;

	k_thread_priority_set(k_current_get(), FR_PRIO(configTIMER_TASK_PRIORITY - 1));

	xPreTime = xTaskGetTickCount();
	k_sleep(K_TICKS(bktTIME_TO_BLOCK));
	xPostTime = xTaskGetTickCount();
	if ((xPostTime - xPreTime) > (bktTIME_TO_BLOCK + xAllowableMargin)) {
		xBktError = __LINE__;
	}

	xPostTime = xTaskGetTickCount();
	xLastUnblockTime = xPostTime;
	for (uint32_t x = 0; x < xCycles; x++) {
		xExpectedUnblockTime = xPostTime + (x * xPeriod);
		xTaskDelayUntil(&xLastUnblockTime, xPeriod);
		if ((xTaskGetTickCount() - xExpectedUnblockTime) >
		    (bktTIME_TO_BLOCK + xAllowableMargin)) {
			xBktError = __LINE__;
		}
		xPrimaryCycles++;
	}

	xDidBlock = xTaskDelayUntil(&xLastUnblockTime, xPeriod);
	if (!xDidBlock) {
		xBktError = __LINE__;
	}
	k_sleep(K_TICKS(xHalfPeriod));
	xDidBlock = xTaskDelayUntil(&xLastUnblockTime, xPeriod);
	if (!xDidBlock) {
		xBktError = __LINE__;
	}
	k_sleep(K_TICKS(xPeriod));
	xDidBlock = xTaskDelayUntil(&xLastUnblockTime, xPeriod);
	if (xDidBlock) {
		xBktError = __LINE__;
	}
	xDidBlock = xTaskDelayUntil(&xLastUnblockTime, xPeriod);
	if (!xDidBlock) {
		xBktError = __LINE__;
	}
	k_sleep(K_TICKS(xPeriod + xAllowableMargin));
	xDidBlock = xTaskDelayUntil(&xLastUnblockTime, xPeriod);
	if (xDidBlock) {
		xBktError = __LINE__;
	}

	k_thread_priority_set(k_current_get(), FR_PRIO(bktPRIMARY_PRIORITY));
}

static void wait_run_indicator(void)
{
	while (xRunIndicator != bktRUN_INDICATOR) {
		k_sleep(K_TICKS(bktSHORT_WAIT));
	}
	k_sleep(K_TICKS(bktSHORT_WAIT));
	xRunIndicator = 0;
}

static void vPrimaryBlockTimeTestTask(void *p1, void *p2, void *p3)
{
	int xItem, xData;
	uint32_t xTimeWhenBlocking, xTimeToBlock;

	for (;;) {
		prvBasicDelayTests();

		/* Receive from the empty queue with growing timeouts. */
		for (xItem = 0; xItem < bktQUEUE_LENGTH; xItem++) {
			xTimeToBlock = bktPRIMARY_BLOCK_TIME << xItem;
			xTimeWhenBlocking = xTaskGetTickCount();
			if (k_msgq_get(&xTestQueue, &xData, K_TICKS(xTimeToBlock)) != -EAGAIN) {
				xBktError = __LINE__;
			}
			check_blocked(xTimeWhenBlocking, xTimeToBlock, __LINE__);
		}

		/* Fill it, then send to the full queue with growing timeouts. */
		for (xItem = 0; xItem < bktQUEUE_LENGTH; xItem++) {
			if (k_msgq_put(&xTestQueue, &xItem, K_NO_WAIT) != 0) {
				xBktError = __LINE__;
			}
		}
		for (xItem = 0; xItem < bktQUEUE_LENGTH; xItem++) {
			xTimeToBlock = bktPRIMARY_BLOCK_TIME << xItem;
			xTimeWhenBlocking = xTaskGetTickCount();
			if (k_msgq_put(&xTestQueue, &xItem, K_TICKS(xTimeToBlock)) != -EAGAIN) {
				xBktError = __LINE__;
			}
			check_blocked(xTimeWhenBlocking, xTimeToBlock, __LINE__);
		}

		/* The secondary blocks sending to the full queue and must time
		 * out. The original then makes room and refills the queue,
		 * with the secondary's priority raised, and checks that the
		 * secondary never gets through: a FreeRTOS queue only readies
		 * a blocked task, which then retries. A k_msgq hands the item
		 * straight to the blocked thread, so that part only runs with
		 * CONFIG_FREERTOS_DEMO_BLOCKTIM_REFILL.
		 */
		xRunIndicator = 0;
		k_thread_resume(xSecondary);
		wait_run_indicator();
		if (IS_ENABLED(CONFIG_FREERTOS_DEMO_BLOCKTIM_REFILL)) {
			for (xItem = 0; xItem < bktQUEUE_LENGTH; xItem++) {
				if (k_msgq_get(&xTestQueue, &xData, K_NO_WAIT) != 0) {
					xBktError = __LINE__;
				}
				if (k_msgq_put(&xTestQueue, &xItem, K_NO_WAIT) != 0) {
					xBktError = __LINE__;
				}
				if (xRunIndicator == bktRUN_INDICATOR) {
					xBktError = __LINE__;
				}
				k_thread_priority_set(xSecondary,
						      FR_PRIO(bktPRIMARY_PRIORITY + 2));
				if (xRunIndicator == bktRUN_INDICATOR) {
					xBktError = __LINE__;
				}
				k_thread_priority_set(xSecondary, FR_PRIO(bktSECONDARY_PRIORITY));
			}
		}
		wait_run_indicator();

		/* The same with the secondary blocked receiving from the
		 * empty queue.
		 */
		for (xItem = 0; xItem < bktQUEUE_LENGTH; xItem++) {
			if (k_msgq_get(&xTestQueue, &xData, K_NO_WAIT) != 0) {
				xBktError = __LINE__;
			}
		}
		k_thread_resume(xSecondary);
		wait_run_indicator();
		if (IS_ENABLED(CONFIG_FREERTOS_DEMO_BLOCKTIM_REFILL)) {
			for (xItem = 0; xItem < bktQUEUE_LENGTH; xItem++) {
				if (k_msgq_put(&xTestQueue, &xItem, K_NO_WAIT) != 0) {
					xBktError = __LINE__;
				}
				if (k_msgq_get(&xTestQueue, &xData, K_NO_WAIT) != 0) {
					xBktError = __LINE__;
				}
				if (xRunIndicator == bktRUN_INDICATOR) {
					xBktError = __LINE__;
				}
				k_thread_priority_set(xSecondary,
						      FR_PRIO(bktPRIMARY_PRIORITY + 2));
				if (xRunIndicator == bktRUN_INDICATOR) {
					xBktError = __LINE__;
				}
				k_thread_priority_set(xSecondary, FR_PRIO(bktSECONDARY_PRIORITY));
			}
		}
		wait_run_indicator();

		xPrimaryCycles++;
	}
}

static void vSecondaryBlockTimeTestTask(void *p1, void *p2, void *p3)
{
	uint32_t xTimeWhenBlocking;
	int xData;

	for (;;) {
		k_thread_suspend(k_current_get());
		xTimeWhenBlocking = xTaskGetTickCount();
		xData = 0;
		xRunIndicator = bktRUN_INDICATOR;
		if (k_msgq_put(&xTestQueue, &xData, K_TICKS(bktTIME_TO_BLOCK)) != -EAGAIN) {
			xBktError = __LINE__;
		}
		check_blocked(xTimeWhenBlocking, bktTIME_TO_BLOCK, __LINE__);
		xRunIndicator = bktRUN_INDICATOR;

		k_thread_suspend(k_current_get());
		xTimeWhenBlocking = xTaskGetTickCount();
		xRunIndicator = bktRUN_INDICATOR;
		if (k_msgq_get(&xTestQueue, &xData, K_TICKS(bktTIME_TO_BLOCK)) != -EAGAIN) {
			xBktError = __LINE__;
		}
		check_blocked(xTimeWhenBlocking, bktTIME_TO_BLOCK, __LINE__);
		xRunIndicator = bktRUN_INDICATOR;
		xSecondaryCycles++;
	}
}

void vCreateBlockTimeTasks(void)
{
	k_msgq_init(&xTestQueue, bkt_buf, sizeof(int), bktQUEUE_LENGTH);
	fr_task_create(vPrimaryBlockTimeTestTask, "BTest1", NULL, bktPRIMARY_PRIORITY, true);
	xSecondary = fr_task_create(vSecondaryBlockTimeTestTask, "BTest2", NULL,
				    bktSECONDARY_PRIORITY, true);
}

bool xAreBlockTimeTestTasksStillRunning(void)
{
	static int xLastPrimaryCycleCount, xLastSecondaryCycleCount;
	bool xReturn = true;

	if ((xPrimaryCycles == xLastPrimaryCycleCount) ||
	    (xSecondaryCycles == xLastSecondaryCycleCount) || (xBktError != 0)) {
		xReturn = false;
	}
	if (xBktError != 0) {
		printk("blocktim error at line %d\n", xBktError);
	}
	xLastSecondaryCycleCount = xSecondaryCycles;
	xLastPrimaryCycleCount = xPrimaryCycles;
	return xReturn;
}
