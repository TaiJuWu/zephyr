/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * k_timer_stop() racing a one-shot expiry: if the timer is popped by the
 * announcing CPU and k_timer_stop() on the other CPU takes timer_lock before
 * the expiration handler does, k_timer_stop() sees the timeout inactive and
 * returns early, and the handler sees it superseded and bails. Neither
 * expiry_fn nor stop_fn runs and a thread in k_timer_status_sync() is never
 * woken.
 *
 * CPU 0 takes the system timer interrupt and runs the waiter; CPU 1 starts
 * the timer and stops it at a random point around its expiry.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define DURATION_TICKS 20	/* 2 ms at 10 kHz */
#define RUN_MS 60000

static struct k_timer tmr;
static atomic_t n_exp, n_stop;
static K_SEM_DEFINE(go, 0, 1);
static K_SEM_DEFINE(done, 0, 1);
static volatile uint32_t sync_ret;

static void exp_fn(struct k_timer *t) { atomic_inc(&n_exp); }
static void stop_fn(struct k_timer *t) { atomic_inc(&n_stop); }

static void waiter(void *a, void *b, void *c)
{
	for (;;) {
		k_sem_take(&go, K_FOREVER);
		sync_ret = k_timer_status_sync(&tmr);
		k_sem_give(&done);
	}
}

static uint32_t rnd;
static uint32_t lcg(void)
{
	rnd = rnd * 1664525U + 1013904223U;
	return rnd >> 8;
}

static void stopper(void *a, void *b, void *c)
{
	uint32_t iters = 0, hang = 0, lost = 0, hang_lost = 0, dbl = 0, by_stop = 0;
	int64_t end = k_uptime_get() + RUN_MS, next_print = k_uptime_get() + 5000;

	rnd = k_cycle_get_32();
	while (k_uptime_get() < end) {
		atomic_set(&n_exp, 0);
		atomic_set(&n_stop, 0);
		k_timer_start(&tmr, K_TICKS(DURATION_TICKS), K_NO_WAIT);
		k_sem_give(&go);
		/* 1.8 to 2.2 ms after the start: around the expiry, with the
		 * waiter long since pended.
		 */
		k_busy_wait(1800 + lcg() % 400);
		k_timer_stop(&tmr);

		bool hung = k_sem_take(&done, K_MSEC(50)) != 0;
		int cb = atomic_get(&n_exp) + atomic_get(&n_stop);

		if (hung) {
			hang++;
			if (cb == 0) {
				hang_lost++;
			}
			if (hang <= 5) {
				printk("HANG #%u at iter %u: expiry_fn %ld, stop_fn %ld\n", hang,
				       iters, atomic_get(&n_exp), atomic_get(&n_stop));
			}
			/* Recover: a fresh start + expiry wakes the waiter. */
			k_timer_start(&tmr, K_TICKS(1), K_NO_WAIT);
			if (k_sem_take(&done, K_MSEC(50)) != 0) {
				printk("waiter not recovered\n");
				break;
			}
		} else if (cb == 0) {
			lost++;
		} else if (cb > 1) {
			dbl++;
		}
		if (atomic_get(&n_stop) != 0) {
			by_stop++;
		}
		iters++;
		if (k_uptime_get() >= next_print) {
			printk("%lld s: iters %u, hangs %u, lost callbacks %u\n",
			       k_uptime_get() / 1000, iters, hang, lost + hang_lost);
			next_print += 5000;
		}
	}
	printk("RESULT iters=%u hangs=%u hangs_without_callback=%u lost_without_hang=%u "
	       "both_callbacks=%u stopped=%u\n", iters, hang, hang_lost, lost, dbl, by_stop);
	printk("PROJECT EXECUTION %s\n", hang || lost || dbl ? "FAILED" : "SUCCESSFUL");
}

K_THREAD_STACK_DEFINE(w_stack, 2048);
K_THREAD_STACK_DEFINE(s_stack, 2048);
static struct k_thread w_thread, s_thread;

int main(void)
{
	printk("timer_stop_race: %d ms one-shot, stop at 1.8-2.2 ms, %d s\n",
	       DURATION_TICKS * 1000 / CONFIG_SYS_CLOCK_TICKS_PER_SEC, RUN_MS / 1000);
	k_timer_init(&tmr, exp_fn, stop_fn);

	k_thread_create(&w_thread, w_stack, K_THREAD_STACK_SIZEOF(w_stack), waiter, NULL, NULL,
			NULL, K_PRIO_PREEMPT(1), 0, K_FOREVER);
	k_thread_cpu_pin(&w_thread, 0);
	k_thread_create(&s_thread, s_stack, K_THREAD_STACK_SIZEOF(s_stack), stopper, NULL, NULL,
			NULL, K_PRIO_PREEMPT(2), 0, K_FOREVER);
	k_thread_cpu_pin(&s_thread, 1);
	k_thread_start(&w_thread);
	k_thread_start(&s_thread);
	return 0;
}
