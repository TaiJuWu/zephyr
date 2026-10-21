/*
 * Repro for the z_try_abort_timeout() retry livelock (wifi-test.md #37),
 * without WiFi.
 *
 * Waiters, kept off the last CPU, k_poll() a signal with a 1-tick timeout, so
 * their timeouts expire together in the timer interrupt. A raiser pinned to
 * the last CPU keeps raising their signals. When a raise reaches a waiter
 * whose timeout handler is running on another CPU, signal_poller() ->
 * z_unpend_thread() retries under the scheduler lock that the handler needs.
 *
 * Reported: the longest k_poll_signal_raise() call and the longest gap
 * between runs of a 1 ms k_timer. A stall is either one over STALL_MS.
 */
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#endif

#define N_WAITERS 8
#define STACK     2048
#define PRIO      K_PRIO_PREEMPT(5)
#define STALL_MS  20

#ifndef TL_SECS
#define TL_SECS 30
#endif

static struct k_poll_signal sigs[N_WAITERS];
static struct k_thread waiters[N_WAITERS];
static K_THREAD_STACK_ARRAY_DEFINE(wstacks, N_WAITERS, STACK);
static struct k_thread raiser;
static K_THREAD_STACK_DEFINE(rstack, STACK);

#ifdef TL_INSTR
/* From the instrumented kernel/sched.c (not upstream). */
extern uint32_t tl_retry_calls, tl_retry_max;
extern uint64_t tl_retry_iters, tl_retry_cyc_max;
static uint32_t ins_calls, ins_max;
static uint64_t ins_iters, ins_cyc_max;
#endif

static volatile bool running;
static atomic_t wakes, timeouts, raises;
static uint64_t max_raise, max_gap, last_tick;
static uint32_t raise_stalls, gap_stalls;

static inline uint64_t cyc_now(void)
{
	return IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER) ? k_cycle_get_64()
								 : k_cycle_get_32();
}

static inline uint64_t cyc_since(uint64_t start)
{
	uint64_t now = cyc_now();

	return IS_ENABLED(CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER)
		       ? now - start
		       : (uint32_t)((uint32_t)now - (uint32_t)start);
}

static inline uint32_t cyc_ms(uint64_t c)
{
	return (uint32_t)k_cyc_to_ms_floor64(c);
}

static void tick_fn(struct k_timer *t)
{
	ARG_UNUSED(t);

	if (last_tick != 0) {
		uint64_t gap = cyc_since(last_tick);

		if (gap > max_gap) {
			max_gap = gap;
		}
		if (cyc_ms(gap) > STALL_MS) {
			gap_stalls++;
		}
	}
	last_tick = cyc_now();
}
static K_TIMER_DEFINE(tick, tick_fn, NULL);

static void waiter(void *p1, void *p2, void *p3)
{
	struct k_poll_signal *sig = p1;
	struct k_poll_event ev = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL,
							  K_POLL_MODE_NOTIFY_ONLY, sig);

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (running) {
		k_poll_signal_reset(sig);
		ev.state = K_POLL_STATE_NOT_READY;
		if (k_poll(&ev, 1, K_TICKS(1)) == 0) {
			atomic_inc(&wakes);
		} else {
			atomic_inc(&timeouts);
		}
	}
}

static void raise_loop(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (int i = 0; running; i = (i + 1) % N_WAITERS) {
		uint64_t t0 = cyc_now();

		k_poll_signal_raise(&sigs[i], 0);

		uint64_t d = cyc_since(t0);

		if (d > max_raise) {
			max_raise = d;
		}
		if (cyc_ms(d) > STALL_MS) {
			raise_stalls++;
			printk("STALL: raise took %u ms at %lld ms\n", cyc_ms(d), k_uptime_get());
		}
		atomic_inc(&raises);
	}
}

static void run(int secs)
{
	unsigned int last = arch_num_cpus() - 1;
	uint32_t total_raise_stalls = 0, total_gap_stalls = 0, worst_raise = 0, worst_gap = 0;

	printk("timeout_livelock: %u CPUs, raiser on CPU %u, %d waiters, %d s, tick %d Hz\n",
	       arch_num_cpus(), last, N_WAITERS, secs, CONFIG_SYS_CLOCK_TICKS_PER_SEC);
	running = true;
	last_tick = 0;
	k_timer_start(&tick, K_MSEC(1), K_MSEC(1));
	for (int i = 0; i < N_WAITERS; i++) {
		k_poll_signal_init(&sigs[i]);
		k_thread_create(&waiters[i], wstacks[i], STACK, waiter, &sigs[i], NULL, NULL,
				PRIO, 0, K_FOREVER);
		k_thread_cpu_mask_disable(&waiters[i], last);
		k_thread_start(&waiters[i]);
	}
	k_thread_create(&raiser, rstack, STACK, raise_loop, NULL, NULL, NULL,
			PRIO, 0, K_FOREVER);
	k_thread_cpu_pin(&raiser, last);
	k_thread_start(&raiser);

	for (int s = 1; s <= secs; s++) {
		k_msleep(1000);

		/* Racy reads and resets: these are statistics. */
		uint32_t mr = cyc_ms(max_raise), mg = cyc_ms(max_gap);

		max_raise = max_gap = 0;
		printk("%3d s: raises=%ld wakes=%ld timeouts=%ld max_raise=%u ms "
		       "max_gap=%u ms stalls=%u/%u\n",
		       s, atomic_clear(&raises), atomic_clear(&wakes), atomic_clear(&timeouts), mr,
		       mg, raise_stalls, gap_stalls);
#ifdef TL_INSTR
		/* Racy against the sched-lock writers: statistics only. */
		uint32_t c = tl_retry_calls, m = tl_retry_max;
		uint64_t it = tl_retry_iters, cy = tl_retry_cyc_max;

		tl_retry_calls = tl_retry_max = 0;
		tl_retry_iters = tl_retry_cyc_max = 0;
		ins_calls += c;
		ins_iters += it;
		ins_max = MAX(ins_max, m);
		ins_cyc_max = MAX(ins_cyc_max, cy);
		printk("%3d s: RETRY calls=%u iters=%llu max_iters=%u max_loop_us=%llu\n", s, c,
		       (unsigned long long)it, m, (unsigned long long)k_cyc_to_us_ceil64(cy));
#endif
		worst_raise = MAX(worst_raise, mr);
		worst_gap = MAX(worst_gap, mg);
	}
	running = false;
	for (int i = 0; i < N_WAITERS; i++) {
		k_thread_join(&waiters[i], K_FOREVER);
	}
	k_thread_join(&raiser, K_FOREVER);
	k_timer_stop(&tick);
	total_raise_stalls = raise_stalls;
	total_gap_stalls = gap_stalls;
	raise_stalls = gap_stalls = 0;
	printk("RESULT: stalls(raise/timer)=%u/%u worst_raise=%u ms worst_gap=%u ms\n",
	       total_raise_stalls, total_gap_stalls, worst_raise, worst_gap);
#ifdef TL_INSTR
	printk("RETRY_RESULT: calls=%u iters=%llu max_iters=%u max_loop_us=%llu\n", ins_calls,
	       (unsigned long long)ins_iters, ins_max,
	       (unsigned long long)k_cyc_to_us_ceil64(ins_cyc_max));
#endif
}

#ifdef CONFIG_SHELL
static int cmd_tl(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);

	run(argc > 1 ? atoi(argv[1]) : TL_SECS);
	return 0;
}
SHELL_CMD_ARG_REGISTER(tl, NULL, "tl [secs]: run the timeout livelock repro", cmd_tl, 1, 1);

int main(void)
{
	return 0;
}
#else
int main(void)
{
	run(TL_SECS);
	printk("DONE\n");
	return 0;
}
#endif
