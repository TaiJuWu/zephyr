/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * semtest.c, countsem.c, recmutex.c and integer.c. Binary and counting
 * semaphores become k_sem, the recursive mutex a k_mutex (recursive, with
 * priority inheritance, like FreeRTOS mutexes), critical sections
 * irq_lock().
 */

#include "demo.h"

/* ---- semtest.c ---- */

#define semtstBLOCKING_EXPECTED_VALUE     0xfffU
#define semtstNON_BLOCKING_EXPECTED_VALUE 0xffU
#define semtstNUM_TASKS                   4
#define semtstDELAY_FACTOR                10

struct xSemaphoreParameters {
	struct k_sem xSemaphore;
	volatile uint32_t ulSharedVariable;
	uint32_t xBlockTime;
};

static struct xSemaphoreParameters sem_params[2];
static volatile short sCheckVariables[semtstNUM_TASKS];
static volatile short sNextCheckVariable;

static void prvSemaphoreTest(void *p1, void *p2, void *p3)
{
	struct xSemaphoreParameters *pxParameters = p1;
	volatile uint32_t *pulSharedVariable = &pxParameters->ulSharedVariable;
	uint32_t ulExpectedValue;
	bool sError = false;
	short sCheckVariableToUse;
	unsigned int key = irq_lock();

	sCheckVariableToUse = sNextCheckVariable;
	sNextCheckVariable++;
	irq_unlock(key);

	ulExpectedValue = (pxParameters->xBlockTime > 0) ? semtstBLOCKING_EXPECTED_VALUE
							  : semtstNON_BLOCKING_EXPECTED_VALUE;

	for (;;) {
		if (k_sem_take(&pxParameters->xSemaphore,
			       K_TICKS(pxParameters->xBlockTime)) == 0) {
			if (*pulSharedVariable != ulExpectedValue) {
				sError = true;
			}
			for (uint32_t ulCounter = 0; ulCounter <= ulExpectedValue; ulCounter++) {
				*pulSharedVariable = ulCounter;
				if (*pulSharedVariable != ulCounter) {
					sError = true;
				}
			}
			/* xSemaphoreGive() of a binary semaphore that is
			 * already given fails; k_sem_give() cannot report it,
			 * so check the count before.
			 */
			if (k_sem_count_get(&pxParameters->xSemaphore) != 0U) {
				sError = true;
			}
			k_sem_give(&pxParameters->xSemaphore);
			if (!sError && (sCheckVariableToUse < semtstNUM_TASKS)) {
				sCheckVariables[sCheckVariableToUse]++;
			}
			if (pxParameters->xBlockTime != 0) {
				k_sleep(K_TICKS(pxParameters->xBlockTime * semtstDELAY_FACTOR));
			}
		} else if (pxParameters->xBlockTime == 0) {
			k_yield();
		}
	}
}

void vStartSemaphoreTasks(int uxPriority)
{
	k_sem_init(&sem_params[0].xSemaphore, 1, 1);
	sem_params[0].ulSharedVariable = semtstNON_BLOCKING_EXPECTED_VALUE;
	sem_params[0].xBlockTime = 0;
	fr_task_create(prvSemaphoreTest, "PolSEM1", &sem_params[0], tskIDLE_PRIORITY, false);
	fr_task_create(prvSemaphoreTest, "PolSEM2", &sem_params[0], tskIDLE_PRIORITY, false);

	k_sem_init(&sem_params[1].xSemaphore, 1, 1);
	sem_params[1].ulSharedVariable = semtstBLOCKING_EXPECTED_VALUE;
	sem_params[1].xBlockTime = 100;	/* xBlockTime / portTICK_PERIOD_MS */
	fr_task_create(prvSemaphoreTest, "BlkSEM1", &sem_params[1], uxPriority, false);
	fr_task_create(prvSemaphoreTest, "BlkSEM2", &sem_params[1], uxPriority, false);
}

bool xAreSemaphoreTasksStillRunning(void)
{
	static short sLastCheckVariables[semtstNUM_TASKS];
	bool xReturn = true;

	for (int xTask = 0; xTask < semtstNUM_TASKS; xTask++) {
		if (sLastCheckVariables[xTask] == sCheckVariables[xTask]) {
			xReturn = false;
		}
		sLastCheckVariables[xTask] = sCheckVariables[xTask];
	}
	return xReturn;
}

/* ---- countsem.c ---- */

#define countMAX_COUNT_VALUE   200
#define countSTART_AT_MAX_COUNT 0xaa
#define countSTART_AT_ZERO      0x55

struct xCountSemStruct {
	struct k_sem xSemaphore;
	unsigned int uxExpectedStartCount;
	volatile unsigned int uxLoopCounter;
};

static volatile bool xCountErrorDetected;
static struct xCountSemStruct xParameters[2];

/* xSemaphoreGive() on a full counting semaphore fails; k_sem_give() at the
 * limit silently does nothing, so report it from the count.
 */
static bool sem_give_ok(struct k_sem *s)
{
	unsigned int before = k_sem_count_get(s);

	k_sem_give(s);
	return k_sem_count_get(s) != before;
}

static void prvIncrementSemaphoreCount(struct k_sem *xSemaphore,
				       volatile unsigned int *puxLoopCounter)
{
	if (k_sem_take(xSemaphore, K_NO_WAIT) == 0) {
		xCountErrorDetected = true;
	}
	for (int ux = 0; ux < countMAX_COUNT_VALUE; ux++) {
		if (!sem_give_ok(xSemaphore)) {
			xCountErrorDetected = true;
		}
		(*puxLoopCounter)++;
	}
	if (sem_give_ok(xSemaphore)) {
		xCountErrorDetected = true;
	}
}

static void prvDecrementSemaphoreCount(struct k_sem *xSemaphore,
				       volatile unsigned int *puxLoopCounter)
{
	if (sem_give_ok(xSemaphore)) {
		xCountErrorDetected = true;
	}
	for (int ux = 0; ux < countMAX_COUNT_VALUE; ux++) {
		if (k_sem_take(xSemaphore, K_NO_WAIT) != 0) {
			xCountErrorDetected = true;
		}
		(*puxLoopCounter)++;
	}
	if (k_sem_take(xSemaphore, K_NO_WAIT) == 0) {
		xCountErrorDetected = true;
	}
}

static void prvCountingSemaphoreTask(void *p1, void *p2, void *p3)
{
	struct xCountSemStruct *pxParameter = p1;

	if (pxParameter->uxExpectedStartCount == countSTART_AT_MAX_COUNT) {
		prvDecrementSemaphoreCount(&pxParameter->xSemaphore, &pxParameter->uxLoopCounter);
	}
	if (k_sem_take(&pxParameter->xSemaphore, K_NO_WAIT) == 0) {
		xCountErrorDetected = true;
	}
	for (;;) {
		prvIncrementSemaphoreCount(&pxParameter->xSemaphore, &pxParameter->uxLoopCounter);
		prvDecrementSemaphoreCount(&pxParameter->xSemaphore, &pxParameter->uxLoopCounter);
	}
}

void vStartCountingSemaphoreTasks(void)
{
	k_sem_init(&xParameters[0].xSemaphore, countMAX_COUNT_VALUE, countMAX_COUNT_VALUE);
	xParameters[0].uxExpectedStartCount = countSTART_AT_MAX_COUNT;
	k_sem_init(&xParameters[1].xSemaphore, 0, countMAX_COUNT_VALUE);
	xParameters[1].uxExpectedStartCount = 0;
	fr_task_create(prvCountingSemaphoreTask, "CNT1", &xParameters[0], tskIDLE_PRIORITY, false);
	fr_task_create(prvCountingSemaphoreTask, "CNT2", &xParameters[1], tskIDLE_PRIORITY, false);
}

bool xAreCountingSemaphoreTasksStillRunning(void)
{
	static unsigned int uxLastCount0, uxLastCount1;
	bool xReturn = !xCountErrorDetected;

	if (uxLastCount0 == xParameters[0].uxLoopCounter) {
		xReturn = false;
	} else {
		uxLastCount0 = xParameters[0].uxLoopCounter;
	}
	if (uxLastCount1 == xParameters[1].uxLoopCounter) {
		xReturn = false;
	} else {
		uxLastCount1 = xParameters[1].uxLoopCounter;
	}
	return xReturn;
}

/* ---- recmutex.c ---- */

#define recmuCONTROLLING_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define recmuBLOCKING_TASK_PRIORITY    (tskIDLE_PRIORITY + 1)
#define recmuPOLLING_TASK_PRIORITY     (tskIDLE_PRIORITY + 0)
#define recmuMAX_COUNT                 10
#define recmuSHORT_DELAY               20
#define recmu15ms_DELAY                15

static struct k_mutex xMutex;
static volatile int xRecError;
static volatile bool xControllingIsSuspended, xBlockingIsSuspended;
static volatile unsigned int uxControllingCycles, uxBlockingCycles, uxPollingCycles;
static k_tid_t xControllingTaskHandle, xBlockingTaskHandle;

static void prvRecursiveMutexControllingTask(void *p1, void *p2, void *p3)
{
	for (;;) {
		/* Giving a mutex we do not hold must fail. */
		if (k_mutex_unlock(&xMutex) == 0) {
			xRecError = __LINE__;
		}
		for (int ux = 0; ux < recmuMAX_COUNT; ux++) {
			if (k_mutex_lock(&xMutex, K_TICKS(recmu15ms_DELAY)) != 0) {
				xRecError = __LINE__;
			}
			k_sleep(K_TICKS(recmuSHORT_DELAY));
		}
		for (int ux = 0; ux < recmuMAX_COUNT; ux++) {
			k_sleep(K_TICKS(recmuSHORT_DELAY));
			if (k_mutex_unlock(&xMutex) != 0) {
				xRecError = __LINE__;
			}
		}
		if (k_mutex_unlock(&xMutex) == 0) {
			xRecError = __LINE__;
		}
		uxControllingCycles++;
		xControllingIsSuspended = true;
		k_thread_suspend(k_current_get());
		xControllingIsSuspended = false;
	}
}

static void prvRecursiveMutexBlockingTask(void *p1, void *p2, void *p3)
{
	for (;;) {
		/* portMAX_DELAY - 1: a long but finite wait */
		if (k_mutex_lock(&xMutex, K_TICKS(UINT32_MAX - 1)) == 0) {
			if (!xControllingIsSuspended) {
				xRecError = __LINE__;
			} else {
				if (k_mutex_unlock(&xMutex) != 0) {
					xRecError = __LINE__;
				}
				xBlockingIsSuspended = true;
				k_thread_suspend(k_current_get());
				xBlockingIsSuspended = false;
			}
		} else {
			xRecError = __LINE__;
		}
		if (uxControllingCycles != (uxBlockingCycles + 1)) {
			xRecError = __LINE__;
		}
		uxBlockingCycles++;
	}
}

static void prvRecursiveMutexPollingTask(void *p1, void *p2, void *p3)
{
	for (;;) {
		if (k_mutex_lock(&xMutex, K_NO_WAIT) == 0) {
			FR_ASSERT(xRecError, fr_is_suspended(xControllingTaskHandle));
			FR_ASSERT(xRecError, fr_is_suspended(xBlockingTaskHandle));
			if (!xBlockingIsSuspended || !xControllingIsSuspended) {
				xRecError = __LINE__;
			} else {
				uxPollingCycles++;
				k_thread_resume(xBlockingTaskHandle);
				k_thread_resume(xControllingTaskHandle);
				if (xBlockingIsSuspended || xControllingIsSuspended) {
					xRecError = __LINE__;
				}
				/* Both now wait on the mutex, so we inherit the
				 * controlling task's priority.
				 */
				FR_ASSERT(xRecError, k_thread_priority_get(k_current_get()) ==
						     FR_PRIO(recmuCONTROLLING_TASK_PRIORITY));
				if (k_mutex_unlock(&xMutex) != 0) {
					xRecError = __LINE__;
				}
				FR_ASSERT(xRecError, k_thread_priority_get(k_current_get()) ==
						     FR_PRIO(recmuPOLLING_TASK_PRIORITY));
			}
		}
	}
}

void vStartRecursiveMutexTasks(void)
{
	k_mutex_init(&xMutex);
	xControllingTaskHandle = fr_task_create(prvRecursiveMutexControllingTask, "Rec1", NULL,
						recmuCONTROLLING_TASK_PRIORITY, true);
	xBlockingTaskHandle = fr_task_create(prvRecursiveMutexBlockingTask, "Rec2", NULL,
					     recmuBLOCKING_TASK_PRIORITY, true);
	fr_task_create(prvRecursiveMutexPollingTask, "Rec3", NULL, recmuPOLLING_TASK_PRIORITY,
		       true);
}

bool xAreRecursiveMutexTasksStillRunning(void)
{
	static unsigned int uxLastControllingCycles, uxLastBlockingCycles, uxLastPollingCycles;

	if (uxLastControllingCycles == uxControllingCycles) {
		xRecError = __LINE__;
	} else {
		uxLastControllingCycles = uxControllingCycles;
	}
	if (uxLastBlockingCycles == uxBlockingCycles) {
		xRecError = __LINE__;
	} else {
		uxLastBlockingCycles = uxBlockingCycles;
	}
	if (uxLastPollingCycles == uxPollingCycles) {
		xRecError = __LINE__;
	} else {
		uxLastPollingCycles = uxPollingCycles;
	}
	if (xRecError != 0) {
		printk("recmutex error at line %d\n", xRecError);
	}
	return xRecError == 0;
}

/* ---- integer.c ---- */

#define intgCONST1          123L
#define intgCONST2          234567L
#define intgCONST3          (-3L)
#define intgCONST4          7L
#define intgEXPECTED_ANSWER (((intgCONST1 + intgCONST2) * intgCONST3) / intgCONST4)
#define intgNUMBER_OF_TASKS 1

static volatile bool xTaskCheck[intgNUMBER_OF_TASKS];

static void vCompeteingIntMathTask(void *p1, void *p2, void *p3)
{
	volatile bool *pxTaskHasExecuted = p1;
	volatile long lValue;
	bool sError = false;

	for (;;) {
		lValue = intgCONST1;
		lValue += intgCONST2;
		lValue *= intgCONST3;
		lValue /= intgCONST4;
		if (lValue != intgEXPECTED_ANSWER) {
			sError = true;
		}
		if (!sError) {
			unsigned int key = irq_lock();

			*pxTaskHasExecuted = true;
			irq_unlock(key);
		}
	}
}

void vStartIntegerMathTasks(int uxPriority)
{
	for (int sTask = 0; sTask < intgNUMBER_OF_TASKS; sTask++) {
		fr_task_create(vCompeteingIntMathTask, "IntMath", (void *)&xTaskCheck[sTask],
			       uxPriority, false);
	}
}

bool xAreIntegerMathsTaskStillRunning(void)
{
	bool xReturn = true;

	for (int sTask = 0; sTask < intgNUMBER_OF_TASKS; sTask++) {
		if (!xTaskCheck[sTask]) {
			xReturn = false;
		}
		xTaskCheck[sTask] = false;
	}
	return xReturn;
}
