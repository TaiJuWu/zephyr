/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PollQ.c and BlockQ.c. FreeRTOS queues become k_msgq.
 */

#include "demo.h"

/* ---- PollQ.c ---- */

#define pollqQUEUE_SIZE        10
#define pollqPRODUCER_DELAY    200
#define pollqCONSUMER_DELAY    (pollqPRODUCER_DELAY - 20)
#define pollqVALUES_TO_PRODUCE 3
#define pollqINITIAL_VALUE     0

static struct k_msgq xPolledQueue;
static char __aligned(4) polled_buf[pollqQUEUE_SIZE * sizeof(uint16_t)];
static volatile int xPollingConsumerCount = pollqINITIAL_VALUE;
static volatile int xPollingProducerCount = pollqINITIAL_VALUE;

static void vPolledQueueProducer(void *p1, void *p2, void *p3)
{
	uint16_t usValue = 0;
	bool xError = false;

	for (;;) {
		for (int xLoop = 0; xLoop < pollqVALUES_TO_PRODUCE; xLoop++) {
			if (k_msgq_put(&xPolledQueue, &usValue, K_NO_WAIT) != 0) {
				xError = true;
			} else {
				if (!xError) {
					xPollingProducerCount++;
				}
				usValue++;
			}
		}
		k_sleep(K_TICKS(pollqPRODUCER_DELAY));
	}
}

static void vPolledQueueConsumer(void *p1, void *p2, void *p3)
{
	uint16_t usData, usExpectedValue = 0;
	bool xError = false;

	for (;;) {
		while (k_msgq_num_used_get(&xPolledQueue) != 0U) {
			if (k_msgq_get(&xPolledQueue, &usData, K_NO_WAIT) == 0) {
				if (usData != usExpectedValue) {
					xError = true;
					usExpectedValue = usData;
				} else if (!xError) {
					xPollingConsumerCount++;
				}
				usExpectedValue++;
			}
		}
		k_sleep(K_TICKS(pollqCONSUMER_DELAY));
	}
}

void vStartPolledQueueTasks(int uxPriority)
{
	k_msgq_init(&xPolledQueue, polled_buf, sizeof(uint16_t), pollqQUEUE_SIZE);
	fr_task_create(vPolledQueueConsumer, "QConsNB", NULL, uxPriority, false);
	fr_task_create(vPolledQueueProducer, "QProdNB", NULL, uxPriority, false);
}

bool xArePollingQueuesStillRunning(void)
{
	bool xReturn = (xPollingConsumerCount != pollqINITIAL_VALUE) &&
		       (xPollingProducerCount != pollqINITIAL_VALUE);

	xPollingConsumerCount = pollqINITIAL_VALUE;
	xPollingProducerCount = pollqINITIAL_VALUE;
	return xReturn;
}

/* ---- BlockQ.c ---- */

#define blckqNUM_TASK_SETS 3
#define blckqSHORT_DELAY   5

struct xBlockingQueueParameters {
	struct k_msgq *xQueue;
	k_timeout_t xBlockTime;
	bool xDontBlock;
	volatile short *psCheckVariable;
};

static volatile short sBlockingConsumerCount[blckqNUM_TASK_SETS];
static volatile short sBlockingProducerCount[blckqNUM_TASK_SETS];
static struct k_msgq bq[3];
static char __aligned(4) bq_buf1[1 * sizeof(uint16_t)];
static char __aligned(4) bq_buf3[1 * sizeof(uint16_t)];
static char __aligned(4) bq_buf5[5 * sizeof(uint16_t)];
static struct xBlockingQueueParameters bqp[6];

static void vBlockingQueueProducer(void *p1, void *p2, void *p3)
{
	struct xBlockingQueueParameters *pxQueueParameters = p1;
	uint16_t usValue = 0;
	bool sErrorEverOccurred = false;

	for (;;) {
		if (k_msgq_put(pxQueueParameters->xQueue, &usValue,
			       pxQueueParameters->xBlockTime) != 0) {
			sErrorEverOccurred = true;
		} else {
			if (!sErrorEverOccurred) {
				(*pxQueueParameters->psCheckVariable)++;
			}
			++usValue;
			/* configNUMBER_OF_CORES > 1 */
			if (IS_ENABLED(CONFIG_SMP) && pxQueueParameters->xDontBlock) {
				k_sleep(K_TICKS(blckqSHORT_DELAY));
			} else if (pxQueueParameters->xDontBlock) {
				/* The demo's configUSE_PREEMPTION == 0 path. A
				 * polling task is preempted by its blocking peer
				 * on every item, and Zephyr restarts the time
				 * slice at every switch-in, so without this it
				 * never yields to other idle-priority tasks.
				 */
				k_yield();
			}
		}
	}
}

static void vBlockingQueueConsumer(void *p1, void *p2, void *p3)
{
	struct xBlockingQueueParameters *pxQueueParameters = p1;
	uint16_t usData, usExpectedValue = 0;
	bool sErrorEverOccurred = false;

	for (;;) {
		if (k_msgq_get(pxQueueParameters->xQueue, &usData,
			       pxQueueParameters->xBlockTime) == 0) {
			if (usData != usExpectedValue) {
				usExpectedValue = usData;
				sErrorEverOccurred = true;
			} else {
				if (!sErrorEverOccurred) {
					(*pxQueueParameters->psCheckVariable)++;
				}
				++usExpectedValue;
			}
			if (IS_ENABLED(CONFIG_SMP) && pxQueueParameters->xDontBlock) {
				k_sleep(K_TICKS(blckqSHORT_DELAY));
			} else if (pxQueueParameters->xDontBlock) {
				k_yield();	/* as in the producer */
			}
		}
	}
}

static void bq_param(int i, struct k_msgq *q, bool block, volatile short *check)
{
	bqp[i].xQueue = q;
	bqp[i].xDontBlock = !block;
	bqp[i].xBlockTime = block ? K_TICKS(1000) : K_NO_WAIT;
	bqp[i].psCheckVariable = check;
}

void vStartBlockingQueueTasks(int uxPriority)
{
	k_msgq_init(&bq[0], bq_buf1, sizeof(uint16_t), 1);
	k_msgq_init(&bq[1], bq_buf3, sizeof(uint16_t), 1);
	k_msgq_init(&bq[2], bq_buf5, sizeof(uint16_t), 5);

	/* Set 1: blocking consumer at uxPriority, polling producer at idle. */
	bq_param(0, &bq[0], true, &sBlockingConsumerCount[0]);
	bq_param(1, &bq[0], false, &sBlockingProducerCount[0]);
	fr_task_create(vBlockingQueueConsumer, "QConsB1", &bqp[0], uxPriority, false);
	fr_task_create(vBlockingQueueProducer, "QProdB2", &bqp[1], tskIDLE_PRIORITY, false);

	/* Set 2: polling consumer at idle, blocking producer at uxPriority. */
	bq_param(2, &bq[1], false, &sBlockingProducerCount[1]);
	bq_param(3, &bq[1], true, &sBlockingConsumerCount[1]);
	fr_task_create(vBlockingQueueConsumer, "QConsB3", &bqp[2], tskIDLE_PRIORITY, false);
	fr_task_create(vBlockingQueueProducer, "QProdB4", &bqp[3], uxPriority, false);

	/* Set 3: both blocking, both at idle. */
	bq_param(4, &bq[2], true, &sBlockingProducerCount[2]);
	bq_param(5, &bq[2], true, &sBlockingConsumerCount[2]);
	fr_task_create(vBlockingQueueProducer, "QProdB5", &bqp[4], tskIDLE_PRIORITY, false);
	fr_task_create(vBlockingQueueConsumer, "QConsB6", &bqp[5], tskIDLE_PRIORITY, false);
}

bool xAreBlockingQueuesStillRunning(void)
{
	static short sLastBlockingConsumerCount[blckqNUM_TASK_SETS];
	static short sLastBlockingProducerCount[blckqNUM_TASK_SETS];
	bool xReturn = true;

	for (int xTasks = 0; xTasks < blckqNUM_TASK_SETS; xTasks++) {
		if (sBlockingConsumerCount[xTasks] == sLastBlockingConsumerCount[xTasks]) {
			xReturn = false;
		}
		sLastBlockingConsumerCount[xTasks] = sBlockingConsumerCount[xTasks];
		if (sBlockingProducerCount[xTasks] == sLastBlockingProducerCount[xTasks]) {
			xReturn = false;
		}
		sLastBlockingProducerCount[xTasks] = sBlockingProducerCount[xTasks];
	}
	return xReturn;
}
