/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ports of RIOT tests/core:
 * - mutex_order: waiters get the mutex in priority order.
 * - thread_msg_block_race: a blocking receive must not return without a
 *   message while a timer interrupt fires at random times.
 * - thread_race: after making a higher priority thread ready and yielding,
 *   the very next instruction must see that the thread has run.
 *
 * RIOT's main thread is the ztest thread here, set to RIOT's
 * THREAD_PRIORITY_MAIN. Both kernels run lower numbers first.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/random/random.h>

#define THREAD_PRIORITY_MAIN 7
#define STACK_SIZE (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

static K_THREAD_STACK_ARRAY_DEFINE(stacks, 5, STACK_SIZE);
static struct k_thread threads[5];
static int saved_prio;

static void *riot_setup(void)
{
	return NULL;
}

static void riot_before(void *f)
{
	ztest_simple_1cpu_before(f);
	saved_prio = k_thread_priority_get(k_current_get());
	k_thread_priority_set(k_current_get(), THREAD_PRIORITY_MAIN);
}

static void riot_after(void *f)
{
	k_thread_priority_set(k_current_get(), saved_prio);
	ztest_simple_1cpu_after(f);
}

ZTEST_SUITE(riot_port, NULL, riot_setup, riot_before, riot_after, NULL);

/* mutex_order */

#define THREAD_NUMOF 5U
static const int prios[THREAD_NUMOF] = { THREAD_PRIORITY_MAIN - 1, 4, 0, 2, 1 };
static struct k_mutex testlock;
static int lock_order[THREAD_NUMOF];
static int lock_ret[THREAD_NUMOF];
static atomic_t lock_idx;

static void lockme(void *p1, void *p2, void *p3)
{
	int prio = k_thread_priority_get(k_current_get());

	printk("T%p (prio %d): trying to lock mutex now\n", k_current_get(), prio);
	int ret = k_mutex_lock(&testlock, K_FOREVER);

	printk("T%p (prio %d): locked mutex now (%d)\n", k_current_get(), prio, ret);
	atomic_val_t i = atomic_inc(&lock_idx);

	lock_order[i] = prio;
	lock_ret[i] = ret;
	k_yield();
	if (ret == 0) {
		k_mutex_unlock(&testlock);
	}
}

ZTEST(riot_port, test_mutex_order)
{
	k_mutex_init(&testlock);
	atomic_set(&lock_idx, 0);

	/* lock mutex, so that spawned threads have to wait */
	k_mutex_lock(&testlock, K_FOREVER);

	for (unsigned int i = 0; i < THREAD_NUMOF; i++) {
		k_thread_create(&threads[i], stacks[i], STACK_SIZE, lockme, NULL,
				NULL, NULL, prios[i], 0, K_NO_WAIT);
	}

	/* In RIOT every new thread preempts main and blocks on the mutex
	 * before the next one is created. Zephyr's priority inheritance
	 * boosts main to the best waiter's priority, so later threads with a
	 * worse priority have not reached the mutex yet. Wait until all are
	 * blocked on it, as in RIOT.
	 */
	for (unsigned int i = 0; i < THREAD_NUMOF; i++) {
		while ((threads[i].base.thread_state & _THREAD_PENDING) == 0U) {
			k_sleep(K_TICKS(1));
		}
	}

	/* allow threads to lock the mutex */
	k_mutex_unlock(&testlock);
	k_mutex_lock(&testlock, K_FOREVER);
	k_mutex_unlock(&testlock);

	/* The pass criterion of tests/01-run.py: strictly rising priority
	 * numbers.
	 */
	zassert_equal(atomic_get(&lock_idx), THREAD_NUMOF);
	for (unsigned int i = 0; i < THREAD_NUMOF; i++) {
		zassert_ok(lock_ret[i], "lock %u (prio %d) returned %d", i,
			   lock_order[i], lock_ret[i]);
	}
	for (unsigned int i = 1; i < THREAD_NUMOF; i++) {
		zassert_true(lock_order[i] > lock_order[i - 1],
			     "lock %u: prio %d after %d", i, lock_order[i],
			     lock_order[i - 1]);
	}

	for (unsigned int i = 0; i < THREAD_NUMOF; i++) {
		k_thread_join(&threads[i], K_FOREVER);
	}
}

/* thread_msg_block_race
 *
 * RIOT's msg_try_send() succeeds only when the receiver is already blocked
 * in msg_receive(); k_mbox_put() with K_NO_WAIT does the same. The random
 * 1-100 us hardware timer becomes a k_timer, so its period is rounded up
 * to ticks.
 */

#define CANARY_TYPE       0x21fd
#define TIMER_TIMEOUT_MIN 1U
#define TIMER_TIMEOUT_MAX 100U

static struct k_mbox mbox;
static struct k_timer race_timer;
static volatile uint32_t sends;

static void sched_next(void)
{
	uint32_t us = TIMER_TIMEOUT_MIN +
		      sys_rand32_get() % (TIMER_TIMEOUT_MAX - TIMER_TIMEOUT_MIN);

	k_timer_start(&race_timer, K_USEC(us), K_NO_WAIT);
}

static void race_timer_fn(struct k_timer *t)
{
	/* just continue rescheduling interrupt triggering at random time */
	sched_next();
}

static void sender(void *p1, void *p2, void *p3)
{
	while (true) {
		uint16_t type = 0;
		struct k_mbox_msg msg = {
			.size = sizeof(type),
			.tx_data = &type,
			.tx_target_thread = K_ANY,
		};

		/* send without blocking */
		(void)k_mbox_put(&mbox, &msg, K_NO_WAIT);
		sends++;
		k_yield();
	}
}

ZTEST(riot_port, test_thread_msg_block_race)
{
	int64_t end = k_uptime_get() + CONFIG_RIOT_PORT_RACE_MS;
	uint32_t received = 0;

	k_mbox_init(&mbox);
	k_timer_init(&race_timer, race_timer_fn, NULL);
	sends = 0;
	sched_next();

	k_thread_create(&threads[0], stacks[0], STACK_SIZE, sender, NULL, NULL,
			NULL, THREAD_PRIORITY_MAIN + 1, 0, K_NO_WAIT);

	/* RIOT runs forever; stop after CONFIG_RIOT_PORT_RACE_MS. */
	while (k_uptime_get() < end) {
		uint16_t type = CANARY_TYPE;
		struct k_mbox_msg msg = {
			.size = sizeof(type),
			.rx_source_thread = K_ANY,
		};

		/* receive blocked */
		zassert_ok(k_mbox_get(&mbox, &msg, &type, K_FOREVER));

		/* check the receive did not return before the sender
		 * copied the message over
		 */
		zassert_not_equal(type, CANARY_TYPE, "Message was not written");
		received++;
	}

	k_thread_abort(&threads[0]);
	k_timer_stop(&race_timer);
	TC_PRINT("received %u of %u sends\n", received, sends);
}

/* thread_race */

static volatile uint8_t irq_occurred;

static void thread_irq_check(void *p1, void *p2, void *p3)
{
	while (true) {
		irq_occurred = 1;
		k_sleep(K_FOREVER);	/* thread_sleep() */
	}
}

/* Like RIOT's sched_set_status(STATUS_RUNNING) under irq_disable(): make
 * the thread ready without switching to it.
 */
static void thread_wake_wo_yield(k_tid_t tid)
{
	k_sched_lock();
	k_wakeup(tid);
}

/* thread_yield_higher() */
static void thread_yield_higher(void)
{
	k_sched_unlock();
}

static void spin(void)
{
	/* Volatile so it is not messed with by optimizations */
	volatile uint8_t i;

	for (i = 0; i < 255; i++) {
	}
}

ZTEST(riot_port, test_thread_race)
{
	volatile uint8_t race_test;
	k_tid_t pid;

	pid = k_thread_create(&threads[0], stacks[0], STACK_SIZE,
			      thread_irq_check, NULL, NULL, NULL,
			      THREAD_PRIORITY_MAIN - 1, 0, K_FOREVER);
	/* THREAD_CREATE_SLEEPING: let it run once and go to sleep. */
	k_thread_start(pid);
	k_yield();

	TC_PRINT("Checking for working context swap\n");
	irq_occurred = 0;
	thread_wake_wo_yield(pid);
	thread_yield_higher();
	/* Delay so we are not testing for race conditions also */
	spin();
	zassert_equal(irq_occurred, 1, "context swap does not work");

	TC_PRINT("Checking for reset of swaps\n");
	irq_occurred = 0;
	thread_wake_wo_yield(pid);
	thread_yield_higher();
	spin();
	zassert_equal(irq_occurred, 1, "swap was not reset");

	TC_PRINT("Checking for context swap race condition\n");
	irq_occurred = 0;
	thread_wake_wo_yield(pid);
	thread_yield_higher();
	/* Race instruction */
	race_test = irq_occurred;
	zassert_equal(race_test, 1, "context swap race");

	k_thread_abort(pid);
}
