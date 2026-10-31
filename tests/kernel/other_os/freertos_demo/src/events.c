/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Port of FreeRTOS/Demo/Common/Minimal/EventGroupsDemo.c on k_event.
 *
 * FreeRTOS -> Zephyr:
 * - xEventGroupWaitBits(clear, all)  k_event_wait{,_all}{,_safe}()
 * - xEventGroupSetBits()             k_event_post()
 * - xEventGroupClearBits()           k_event_clear()
 * - xEventGroupGetBits(), SetBits(0) k_event_test(~0)
 * - *FromISR() from the tick hook    the same calls from a one-tick k_timer
 * - xEventGroupSync()                eg_sync() below
 * - eTaskGetState()                  fr_state() below
 * Differences (see README.rst):
 * - xEventGroupWaitBits() returns the whole group as it was when the
 *   task unblocked; k_event_wait*() returns the requested bits that
 *   matched. Checks of the return value compare against the requested bits.
 * - vEventGroupDelete() unblocks its waiters with 0. A k_event cannot be
 *   deleted: "deleting" posts EG_DELETED, which the waits that the demo
 *   expects to end that way also wait for, and which then reads as 0.
 * - xEventGroupSetBits() and xEventGroupClearBits() return the group after
 *   the set or before the clear; the checks read it with k_event_test().
 */

#include <zephyr/kernel.h>
#include "demo.h"

#define ebSET_BIT_TASK_PRIORITY      (tskIDLE_PRIORITY)
#define ebWAIT_BIT_TASK_PRIORITY     (tskIDLE_PRIORITY + 1)

#define ebBIT_0 0x01
#define ebBIT_1 0x02
#define ebBIT_2 0x04
#define ebBIT_3 0x08
#define ebBIT_4 0x10
#define ebBIT_5 0x20
#define ebBIT_6 0x40
#define ebBIT_7 0x80

#define ebCOMBINED_BITS (ebBIT_1 | ebBIT_5 | ebBIT_7)
#define ebALL_BITS      (ebBIT_0 | ebBIT_1 | ebBIT_2 | ebBIT_3 | ebBIT_4 | ebBIT_5 | \
			 ebBIT_6 | ebBIT_7)

#define ebSET_BIT_TASK_SYNC_BIT      ebBIT_0
#define ebWAIT_BIT_TASK_SYNC_BIT     ebBIT_1
#define ebRENDEZVOUS_TASK_1_SYNC_BIT ebBIT_2
#define ebRENDEZVOUS_TASK_2_SYNC_BIT ebBIT_3
#define ebALL_SYNC_BITS              (ebSET_BIT_TASK_SYNC_BIT | ebWAIT_BIT_TASK_SYNC_BIT | \
				      ebRENDEZVOUS_TASK_1_SYNC_BIT | \
				      ebRENDEZVOUS_TASK_2_SYNC_BIT)

#define ebSELECTIVE_BITS_1 0x03
#define ebSELECTIVE_BITS_2 0x05

/* Not one of the demo's bits: marks a "deleted" group */
#define EG_DELETED ((uint32_t)BIT(24))

enum fr_state { eReady, eBlocked, eSuspended };

static volatile uint32_t ulTestMasterCycles, ulTestSlaveCycles, ulISRCycles;
static struct k_event xEventGroup;
static struct k_event xISREventGroup;
static k_tid_t xSyncTask1, xSyncTask2;
static volatile int eg_error_line;

#define EG_ERROR()                                                           \
	do {                                                                 \
		if (eg_error_line == 0) {                                    \
			eg_error_line = __LINE__;                            \
			printk("EventGroups: error at line %d\n", __LINE__); \
		}                                                            \
	} while (false)

static enum fr_state fr_state(k_tid_t t)
{
	uint8_t s = t->base.thread_state;

	if ((s & _THREAD_SUSPENDED) != 0U) {
		return eSuspended;
	}
	if ((s & (_THREAD_PENDING | _THREAD_SLEEPING)) != 0U) {
		return eBlocked;
	}
	return eReady;
}

static uint32_t eg_wait_bits(struct k_event *g, uint32_t bits, bool clear, bool all,
			     k_timeout_t timeout)
{
	if (all) {
		return clear ? k_event_wait_all_safe(g, bits, false, timeout)
			     : k_event_wait_all(g, bits, false, timeout);
	}
	return clear ? k_event_wait_safe(g, bits, false, timeout)
		     : k_event_wait(g, bits, false, timeout);
}

static uint32_t eg_get_bits(struct k_event *g)
{
	return k_event_test(g, ~EG_DELETED);
}

/* xEventGroupSync(): set bits, then wait for all of wait_bits, which are
 * cleared once the rendezvous completes. The set, the check and the start
 * of the wait must not be interleaved with another task's, so they run
 * with the scheduler locked (the tasks share one CPU, see demo.h).
 * Waiters that unblock consume the bits themselves (_safe); the task that
 * completes the rendezvous clears them if no one else was waiting.
 */
static uint32_t eg_sync(struct k_event *g, uint32_t set, uint32_t wait_bits,
			k_timeout_t timeout)
{
	uint32_t ret;

	k_sched_lock();
	/* The post below may wake the other tasks, which then consume the
	 * bits: decide from the group as it is before the post.
	 */
	ret = k_event_test(g, ~EG_DELETED) | set;
	k_event_post(g, set);
	if ((ret & wait_bits) == wait_bits) {
		k_event_clear(g, wait_bits);
	} else {
		ret = k_event_wait_all_safe(g, wait_bits, false, timeout);
		if (ret == 0) {
			/* Timed out: the group as it is, with this task's bit */
			ret = k_event_test(g, ~EG_DELETED);
		}
	}
	k_sched_unlock();
	return ret;
}

/* vEventGroupDelete() with tasks blocked on the group */
static void eg_delete(struct k_event *g)
{
	k_event_post(g, EG_DELETED);
}

static void prvSelectiveBitsTestSlaveFunction(void)
{
	uint32_t uxPendBits, uxReturned;

	uxPendBits = (k_current_get() == xSyncTask1) ? ebSELECTIVE_BITS_1 : ebSELECTIVE_BITS_2;

	for (;;) {
		k_thread_suspend(k_current_get());
		uxReturned = eg_wait_bits(&xEventGroup, uxPendBits | EG_DELETED, true, false,
					  K_FOREVER);
		if ((uxReturned & EG_DELETED) != 0U) {
			break;
		}
	}
}

static bool prvSelectiveBitsTestMasterFunction(void)
{
	bool xError = false;

	if (fr_state(xSyncTask1) != eSuspended) {
		xError = true;
	}
	if (fr_state(xSyncTask2) != eSuspended) {
		xError = true;
	}

	for (uint32_t uxBit = 0x01; uxBit < 0x100; uxBit <<= 1) {
		k_thread_resume(xSyncTask1);
		k_thread_resume(xSyncTask2);
		if (fr_state(xSyncTask1) != eBlocked) {
			xError = true;
		}
		if (fr_state(xSyncTask2) != eBlocked) {
			xError = true;
		}

		k_event_post(&xEventGroup, uxBit);

		if ((uxBit & ebSELECTIVE_BITS_1) == 0) {
			if (fr_state(xSyncTask1) != eBlocked) {
				xError = true;
			}
		} else if (fr_state(xSyncTask1) != eSuspended) {
			xError = true;
		}
		if ((uxBit & ebSELECTIVE_BITS_2) == 0) {
			if (fr_state(xSyncTask2) != eBlocked) {
				xError = true;
			}
		} else if (fr_state(xSyncTask2) != eSuspended) {
			xError = true;
		}
	}

	k_thread_resume(xSyncTask1);
	k_thread_resume(xSyncTask2);
	eg_delete(&xEventGroup);
	if (xError) {
		EG_ERROR();
	}
	return xError;
}

static void prvSyncTask(void *p1, void *p2, void *p3)
{
	uint32_t uxSynchronisationBit = POINTER_TO_UINT(p1);
	uint32_t uxReturned;

	prvSelectiveBitsTestSlaveFunction();

	for (;;) {
		k_thread_suspend(k_current_get());

		/* The test master has a lower priority: not complete yet */
		uxReturned = eg_sync(&xEventGroup, uxSynchronisationBit, ebALL_SYNC_BITS,
				     K_NO_WAIT);
		if ((uxReturned & ebALL_SYNC_BITS) == ebALL_SYNC_BITS) {
			EG_ERROR();
		}
		uxReturned = eg_sync(&xEventGroup, uxSynchronisationBit, ebALL_SYNC_BITS,
				     K_FOREVER);
		if ((uxReturned & ebALL_SYNC_BITS) != ebALL_SYNC_BITS) {
			EG_ERROR();
		}

		k_thread_suspend(k_current_get());

		/* This time the test master gets to the sync point first */
		uxReturned = eg_sync(&xEventGroup, uxSynchronisationBit, ebALL_SYNC_BITS,
				     K_FOREVER);
		if ((uxReturned & ebALL_SYNC_BITS) != ebALL_SYNC_BITS) {
			EG_ERROR();
		}

		/* Blocked while the group is deleted: returns 0 */
		uxReturned = eg_wait_bits(&xEventGroup, EG_DELETED, false, true, K_FOREVER);
		if (uxReturned != EG_DELETED) {
			EG_ERROR();
		}
	}
}

static void prvTestSlaveTask(void *p1, void *p2, void *p3)
{
	uint32_t uxReturned;
	bool xError = false;

	for (;;) {
		/* Part 1: counterpart of prvBitCombinationTestMasterFunction() */
		k_thread_suspend(k_current_get());

		/* FreeRTOS returns ebCOMBINED_BITS, the whole group */
		uxReturned = eg_wait_bits(&xEventGroup, ebBIT_1, true, true, K_FOREVER);
		if (uxReturned != ebBIT_1) {
			xError = true;
		}

		uxReturned = eg_wait_bits(&xEventGroup, ebCOMBINED_BITS, false, true, K_FOREVER);
		if ((uxReturned & ebCOMBINED_BITS) != ebCOMBINED_BITS) {
			xError = true;
		}

		k_thread_suspend(k_current_get());

		/* FreeRTOS returns ebALL_BITS, the whole group */
		uxReturned = eg_wait_bits(&xEventGroup, ebCOMBINED_BITS, true, true, K_FOREVER);
		if (uxReturned != ebCOMBINED_BITS) {
			xError = true;
		}

		/* Part 2: counterpart of prvPerformTaskSyncTests() */
		k_thread_suspend(k_current_get());

		uxReturned = eg_sync(&xEventGroup, ebWAIT_BIT_TASK_SYNC_BIT, ebALL_SYNC_BITS,
				     K_FOREVER);
		if ((uxReturned & ebALL_SYNC_BITS) != ebALL_SYNC_BITS) {
			xError = true;
		}
		if (eg_get_bits(&xEventGroup) != 0) {
			xError = true;
		}

		if (!xError) {
			ulTestSlaveCycles++;
		}

		k_thread_suspend(k_current_get());

		uxReturned = eg_sync(&xEventGroup, ebWAIT_BIT_TASK_SYNC_BIT, ebALL_SYNC_BITS,
				     K_FOREVER);
		if ((uxReturned & ebALL_SYNC_BITS) != ebALL_SYNC_BITS) {
			xError = true;
		}
		if (eg_get_bits(&xEventGroup) != 0) {
			xError = true;
		}

		uxReturned = eg_wait_bits(&xEventGroup, EG_DELETED, false, true, K_FOREVER);
		if (uxReturned != EG_DELETED) {
			xError = true;
		}

		if (!xError) {
			ulTestSlaveCycles++;
		} else {
			EG_ERROR();
		}
	}
}

static bool prvPerformTaskSyncTests(bool xError, k_tid_t xTestSlaveTaskHandle)
{
	uint32_t uxBits;

	if (fr_state(xTestSlaveTaskHandle) != eSuspended) {
		xError = true;
	}
	if (fr_state(xSyncTask1) != eSuspended) {
		xError = true;
	}
	if (fr_state(xSyncTask2) != eSuspended) {
		xError = true;
	}

	/* Sync on this task's own bit only */
	k_event_post(&xEventGroup, ebALL_SYNC_BITS & ~ebSET_BIT_TASK_SYNC_BIT);
	uxBits = eg_sync(&xEventGroup, ebSET_BIT_TASK_SYNC_BIT, ebSET_BIT_TASK_SYNC_BIT,
			 K_FOREVER);
	if ((uxBits & ebSET_BIT_TASK_SYNC_BIT) != ebSET_BIT_TASK_SYNC_BIT) {
		xError = true;
	}
	if (eg_get_bits(&xEventGroup) != (ebALL_SYNC_BITS & ~ebSET_BIT_TASK_SYNC_BIT)) {
		xError = true;
	}
	k_event_clear(&xEventGroup, ebALL_SYNC_BITS & ~ebSET_BIT_TASK_SYNC_BIT);
	if (eg_get_bits(&xEventGroup) != 0) {
		xError = true;
	}

	/* This task has the lowest priority: the others get there first */
	k_thread_resume(xTestSlaveTaskHandle);
	k_thread_resume(xSyncTask1);
	k_thread_resume(xSyncTask2);
	if (fr_state(xTestSlaveTaskHandle) != eBlocked) {
		xError = true;
	}
	if (fr_state(xSyncTask1) != eBlocked) {
		xError = true;
	}
	if (fr_state(xSyncTask2) != eBlocked) {
		xError = true;
	}

	uxBits = eg_sync(&xEventGroup, ebSET_BIT_TASK_SYNC_BIT, ebALL_SYNC_BITS, K_FOREVER);
	if ((uxBits & ebALL_SYNC_BITS) != ebALL_SYNC_BITS) {
		xError = true;
	}
	if (eg_get_bits(&xEventGroup) != 0) {
		xError = true;
	}
	if (fr_state(xTestSlaveTaskHandle) != eSuspended) {
		xError = true;
	}
	if (fr_state(xSyncTask1) != eSuspended) {
		xError = true;
	}
	if (fr_state(xSyncTask2) != eSuspended) {
		xError = true;
	}

	/* Now this task gets to the sync point first */
	k_thread_resume(xTestSlaveTaskHandle);
	k_thread_resume(xSyncTask1);
	k_thread_resume(xSyncTask2);
	if (fr_state(xTestSlaveTaskHandle) != eBlocked) {
		xError = true;
	}
	if (fr_state(xSyncTask1) != eBlocked) {
		xError = true;
	}
	if (fr_state(xSyncTask2) != eBlocked) {
		xError = true;
	}

	k_thread_priority_set(k_current_get(), FR_PRIO(ebWAIT_BIT_TASK_PRIORITY + 1));

	uxBits = eg_sync(&xEventGroup, ebSET_BIT_TASK_SYNC_BIT, ebALL_SYNC_BITS, K_FOREVER);
	if ((uxBits & ebALL_SYNC_BITS) != ebALL_SYNC_BITS) {
		xError = true;
	}
	if (eg_get_bits(&xEventGroup) != 0) {
		xError = true;
	}
	/* Unblocked, but with a lower priority than this task */
	if (fr_state(xTestSlaveTaskHandle) != eReady) {
		xError = true;
	}
	if (fr_state(xSyncTask1) != eReady) {
		xError = true;
	}
	if (fr_state(xSyncTask2) != eReady) {
		xError = true;
	}

	k_thread_priority_set(k_current_get(), FR_PRIO(ebSET_BIT_TASK_PRIORITY));

	/* They ran and blocked on the group, which will be deleted */
	if (fr_state(xTestSlaveTaskHandle) != eBlocked) {
		xError = true;
	}
	if (fr_state(xSyncTask1) != eBlocked) {
		xError = true;
	}
	if (fr_state(xSyncTask2) != eBlocked) {
		xError = true;
	}

	if (xError) {
		EG_ERROR();
	}
	return xError;
}

static bool prvBitCombinationTestMasterFunction(bool xError, k_tid_t xTestSlaveTaskHandle)
{
	uint32_t uxBits;

	k_thread_resume(xTestSlaveTaskHandle);
	if (fr_state(xTestSlaveTaskHandle) != eBlocked) {
		xError = true;
	}

	k_event_post(&xEventGroup, ebCOMBINED_BITS);
	uxBits = eg_wait_bits(&xEventGroup, ebALL_BITS, false, false, K_NO_WAIT);
	if (uxBits != (ebCOMBINED_BITS & ~ebBIT_1)) {
		xError = true;
	}
	if (fr_state(xTestSlaveTaskHandle) != eBlocked) {
		xError = true;
	}

	k_event_post(&xEventGroup, ebALL_BITS & ~ebBIT_1);
	uxBits = eg_wait_bits(&xEventGroup, ebALL_BITS, false, false, K_NO_WAIT);
	if (uxBits != (ebALL_BITS & ~ebBIT_1)) {
		xError = true;
	}
	if (fr_state(xTestSlaveTaskHandle) != eBlocked) {
		xError = true;
	}

	k_event_post(&xEventGroup, ebBIT_1);
	if (fr_state(xTestSlaveTaskHandle) != eSuspended) {
		xError = true;
	}

	/* xEventGroupSetBits(0) and xEventGroupClearBits() return the group */
	if (eg_get_bits(&xEventGroup) != ebALL_BITS) {
		xError = true;
	}
	k_sched_lock();
	if (eg_get_bits(&xEventGroup) != ebALL_BITS) {
		xError = true;
	}
	k_event_clear(&xEventGroup, ebBIT_1);
	k_sched_unlock();

	k_thread_resume(xTestSlaveTaskHandle);
	if (fr_state(xTestSlaveTaskHandle) != eBlocked) {
		xError = true;
	}

	k_event_post(&xEventGroup, ebBIT_1);
	if (fr_state(xTestSlaveTaskHandle) != eSuspended) {
		xError = true;
	}

	uxBits = eg_wait_bits(&xEventGroup, ebALL_BITS, false, false, K_NO_WAIT);
	if (uxBits != (ebALL_BITS & ~ebCOMBINED_BITS)) {
		xError = true;
	}
	k_sched_lock();
	if (eg_get_bits(&xEventGroup) != (ebALL_BITS & ~ebCOMBINED_BITS)) {
		xError = true;
	}
	k_event_clear(&xEventGroup, ebALL_BITS);
	k_sched_unlock();
	if (eg_get_bits(&xEventGroup) != 0x00) {
		xError = true;
	}

	if (xError) {
		EG_ERROR();
	}
	return xError;
}

static void prvTestMasterTask(void *p1, void *p2, void *p3)
{
	k_tid_t xTestSlaveTaskHandle = p1;
	bool xError;

	k_event_init(&xEventGroup);
	xError = prvSelectiveBitsTestMasterFunction();

	for (;;) {
		/* Recreate the event group */
		k_event_init(&xEventGroup);

		xError = prvBitCombinationTestMasterFunction(xError, xTestSlaveTaskHandle);
		xError = prvPerformTaskSyncTests(xError, xTestSlaveTaskHandle);

		eg_delete(&xEventGroup);

		/* The other tasks completed and suspended themselves */
		if (fr_state(xTestSlaveTaskHandle) != eSuspended) {
			xError = true;
		}
		if (fr_state(xSyncTask1) != eSuspended) {
			xError = true;
		}
		if (fr_state(xSyncTask2) != eSuspended) {
			xError = true;
		}

		if (!xError) {
			ulTestMasterCycles++;
		} else {
			EG_ERROR();
		}
	}
}

/* vPeriodicEventGroupsProcessing(), called from the tick hook */
static void periodic_event_groups(struct k_timer *timer)
{
	static int xCallCount;
	static bool xISRTestError;
	const int xSetBitCount = 100, xGetBitsCount = 200, xClearBitsCount = 300;
	const uint32_t uxBitsToSet = 0x12U;

	xCallCount++;

	if (xCallCount == xSetBitCount) {
		if (k_event_test(&xISREventGroup, ~0U) != 0x00) {
			xISRTestError = true;
		} else {
			k_event_post(&xISREventGroup, uxBitsToSet);
		}
	} else if (xCallCount == xGetBitsCount) {
		if (k_event_test(&xISREventGroup, ~0U) != uxBitsToSet) {
			xISRTestError = true;
		}
	} else if (xCallCount == xClearBitsCount) {
		k_event_clear(&xISREventGroup, uxBitsToSet);
		xCallCount = 0;
		if (!xISRTestError) {
			ulISRCycles++;
		} else {
			EG_ERROR();
		}
	}
}

static K_TIMER_DEFINE(tick_hook, periodic_event_groups, NULL);

void vStartEventGroupTasks(void)
{
	k_tid_t xTestSlaveTaskHandle;

	/* Created in the demo's order; all on CPU 0 on SMP (demo.h) */
	xTestSlaveTaskHandle = fr_task_create(prvTestSlaveTask, "WaitO", NULL,
					      ebWAIT_BIT_TASK_PRIORITY, true);
	xSyncTask1 = fr_task_create(prvSyncTask, "Rndv",
				    UINT_TO_POINTER(ebRENDEZVOUS_TASK_1_SYNC_BIT),
				    ebWAIT_BIT_TASK_PRIORITY, true);
	xSyncTask2 = fr_task_create(prvSyncTask, "Rndv",
				    UINT_TO_POINTER(ebRENDEZVOUS_TASK_2_SYNC_BIT),
				    ebWAIT_BIT_TASK_PRIORITY, true);
	/* The master is created after the slave in the demo; it starts last
	 * here so that the sync tasks it checks for already exist.
	 */
	fr_task_create(prvTestMasterTask, "SetB", xTestSlaveTaskHandle,
		       ebSET_BIT_TASK_PRIORITY, true);

	k_event_init(&xISREventGroup);
	k_timer_start(&tick_hook, K_TICKS(1), K_TICKS(1));
}

bool xAreEventGroupTasksStillRunning(void)
{
	static uint32_t ulPreviousWaitBitCycles, ulPreviousSetBitCycles, ulPreviousISRCycles;
	bool xStatus = true;

	if (ulPreviousSetBitCycles == ulTestMasterCycles) {
		xStatus = false;
	}
	ulPreviousSetBitCycles = ulTestMasterCycles;
	if (ulPreviousWaitBitCycles == ulTestSlaveCycles) {
		xStatus = false;
	}
	ulPreviousWaitBitCycles = ulTestSlaveCycles;
	if (ulPreviousISRCycles == ulISRCycles) {
		xStatus = false;
	}
	ulPreviousISRCycles = ulISRCycles;

	if (eg_error_line != 0) {
		printk("EventGroups: first error at line %d\n", eg_error_line);
		xStatus = false;
	}
	return xStatus;
}
