/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Scheduler stress tests that follow the Linux tools of the same name:
 * hackbench, schbench, cyclictest, locktorture, pi_stress (rt-tests) and the
 * stress-ng fork, kill and affinity stressors, plus a SCHED_RR fairness
 * check. Each scenario keeps the original tool's semantics; the deviations
 * are listed in README.rst. Scenario 10 has no Linux counterpart.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

#define DUR_MS   CONFIG_SCHED_TORTURE_DURATION_MS
#define STALL_MS CONFIG_SCHED_TORTURE_STALL_MS
#define NTHREADS CONFIG_SCHED_TORTURE_MAX_THREADS
#define STACK    1536

K_THREAD_STACK_ARRAY_DEFINE(stacks, NTHREADS, STACK);
static struct k_thread threads[NTHREADS];
K_THREAD_STACK_DEFINE(mon_stack, 1024);
static struct k_thread mon_thread;

static volatile bool stop;

/* Skip all but the scenario selected with CONFIG_SCHED_TORTURE_ONLY. */
#define ONLY(n)                                                                                    \
	do {                                                                                       \
		if ((CONFIG_SCHED_TORTURE_ONLY != 0) && (CONFIG_SCHED_TORTURE_ONLY != (n))) {      \
			ztest_test_skip();                                                         \
		}                                                                                  \
	} while (false)

static uint32_t rnd(uint32_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 17;
	*s ^= *s << 5;
	return *s;
}

/* Run seed: CONFIG_SCHED_TORTURE_SEED, or a new one each boot (printed, so a run
 * can be replayed). Every thread mixes it into its own xorshift state.
 */
static uint32_t seed;

static uint32_t seed_mix(uint32_t c)
{
	uint32_t s = c ^ seed;

	return (s != 0U) ? s : 1U;
}

/* locktorture knobs; randomized per run with
 * CONFIG_SCHED_TORTURE_RANDOM_PARAMS, as FreeBSD stress2 does.
 */
static uint32_t lt_long_hold_ms = CONFIG_SCHED_TORTURE_LT_LONG_HOLD_MS;
static uint32_t lt_stutter_ms = CONFIG_SCHED_TORTURE_LT_STUTTER_MS;
static uint32_t lt_shuffle_ms = CONFIG_SCHED_TORTURE_LT_SHUFFLE_MS;

static void random_params(void)
{
	uint32_t s = seed_mix(0x2545f491u);

	lt_long_hold_ms = 1 + rnd(&s) % (2 * CONFIG_SCHED_TORTURE_LT_LONG_HOLD_MS + 1);
	lt_stutter_ms = (rnd(&s) % 4 == 0)
				? 0
				: 100 + rnd(&s) % (2 * CONFIG_SCHED_TORTURE_LT_STUTTER_MS + 1);
	lt_shuffle_ms = (rnd(&s) % 4 == 0)
				? 0
				: 50 + rnd(&s) % (2 * CONFIG_SCHED_TORTURE_LT_SHUFFLE_MS + 1);
	printk("random params: long_hold %u ms, stutter %u ms, shuffle %u ms\n",
	       lt_long_hold_ms, lt_stutter_ms, lt_shuffle_ms);
}

static void *seed_setup(void)
{
	seed = CONFIG_SCHED_TORTURE_SEED;
	if (seed == 0U) {
		seed = k_cycle_get_32() * 0x9e3779b9u;
	}
	printk("sched-torture seed 0x%08x (CONFIG_SCHED_TORTURE_SEED=0x%08x replays it)\n", seed,
	       seed);
	if (IS_ENABLED(CONFIG_SCHED_TORTURE_RANDOM_PARAMS)) {
		random_params();
	}
	return NULL;
}

static uint32_t cyc_to_us(uint32_t c)
{
	return (uint32_t)((uint64_t)c * 1000000U / sys_clock_hw_cycles_per_sec());
}

/* CPU the caller runs on; local interrupts are masked so it cannot migrate meanwhile */
static int cur_cpu(void)
{
	unsigned int key = arch_irq_lock();
	int cpu = _current_cpu->id;

	arch_irq_unlock(key);
	return cpu;
}

static int ncpus(void)
{
	return (int)arch_num_cpus();
}

/* Latency statistics: log2 histogram in microseconds. */
struct lat {
	uint32_t n, min_us, max_us;
	uint64_t sum_us;
	uint32_t bucket[32];
};

static void lat_add(struct lat *l, uint32_t us)
{
	int b = us ? 32 - __builtin_clz(us) : 0;

	l->min_us = l->n ? MIN(l->min_us, us) : us;
	l->n++;
	l->sum_us += us;
	l->max_us = MAX(l->max_us, us);
	l->bucket[MIN(b, 31)]++;
}

static void lat_merge(struct lat *dst, const struct lat *src)
{
	if (src->n == 0) {
		return;
	}
	dst->min_us = dst->n ? MIN(dst->min_us, src->min_us) : src->min_us;
	dst->n += src->n;
	dst->sum_us += src->sum_us;
	dst->max_us = MAX(dst->max_us, src->max_us);
	for (int i = 0; i < 32; i++) {
		dst->bucket[i] += src->bucket[i];
	}
}

/* Upper bound of the bucket holding the given per-mille percentile. */
static uint32_t lat_pct(const struct lat *l, uint32_t permille)
{
	uint64_t want = ((uint64_t)l->n * permille + 999) / 1000, seen = 0;

	for (int i = 0; i < 32; i++) {
		seen += l->bucket[i];
		if (seen >= want) {
			return i ? (1U << i) - 1 : 0;
		}
	}
	return l->max_us;
}

/* ---- progress monitor: like rcutorture's stall warnings ---- */
static volatile uint32_t progress[NTHREADS];
static int nmon;
static volatile uint32_t stalls;

static void monitor_fn(void *a, void *b, void *c)
{
	uint32_t last[NTHREADS];
	int64_t since[NTHREADS];
	bool reported[NTHREADS] = {0};

	for (int i = 0; i < nmon; i++) {
		last[i] = progress[i];
		since[i] = k_uptime_get();
	}
	while (!stop) {
		k_msleep(100);

		int64_t now = k_uptime_get();

		for (int i = 0; i < nmon; i++) {
			if (progress[i] != last[i]) {
				last[i] = progress[i];
				since[i] = now;
			} else if (!reported[i] && now - since[i] > STALL_MS) {
				printk("ST stall: thread %d made no progress for %lld ms\n", i,
				       now - since[i]);
				reported[i] = true;
				stalls++;
			}
		}
	}
}

/* Threads 0..n-1 are watched. Call before creating them. */
static void monitor_start(int n)
{
	nmon = n;
	stalls = 0;
	memset((void *)progress, 0, sizeof(progress));
	k_thread_create(&mon_thread, mon_stack, K_THREAD_STACK_SIZEOF(mon_stack), monitor_fn,
			NULL, NULL, NULL, K_PRIO_COOP(1), 0, K_NO_WAIT);
}

/* Sleep while the watched threads are paused on purpose. */
static void monitor_pause(int ms)
{
	int64_t end = k_uptime_get() + ms;

	while (k_uptime_get() < end && !stop) {
		for (int i = 0; i < nmon; i++) {
			progress[i]++;
		}
		k_msleep(50);
	}
}

static k_tid_t spawn(int i, k_thread_entry_t fn, int prio, int cpu)
{
	k_thread_create(&threads[i], stacks[i], STACK, fn, (void *)(uintptr_t)i, NULL, NULL, prio,
			0, K_FOREVER);
#ifdef CONFIG_SCHED_CPU_MASK
	if (cpu >= 0 && ncpus() > 1) {
		k_thread_cpu_pin(&threads[i], cpu);
	}
#else
	ARG_UNUSED(cpu);
#endif
	k_thread_start(&threads[i]);
	return &threads[i];
}

static void join(int from, int to)
{
	for (int i = from; i < to; i++) {
		k_thread_join(&threads[i], K_FOREVER);
	}
}

static void stop_and_join(int n)
{
	stop = true;
	join(0, n);
	k_thread_join(&mon_thread, K_FOREVER);
}

static void before(void *f)
{
	stop = false;
}

/* Optional background CPU hog at the lowest preemptible priority. */
static void hog(void *a, void *b, void *c)
{
	while (!stop) {
		k_busy_wait(200);
	}
}

/* ---------------------------------------------------------------------------
 * 1. hackbench: groups of senders and receivers; every sender sends `loops`
 * messages of `datasize` bytes to every receiver of its group. All threads
 * share one priority, as hackbench's processes share one nice level.
 */
#define HB_FDS    CONFIG_SCHED_TORTURE_HACKBENCH_FDS
#define HB_LOOPS  CONFIG_SCHED_TORTURE_HACKBENCH_LOOPS
#define HB_DATA   100
#define HB_MAXGRP (NTHREADS / (2 * HB_FDS))

struct hb_msg {
	uint16_t sender;
	uint16_t pad;
	uint32_t seq;
	uint8_t data[HB_DATA];
};

static struct k_msgq hbq[HB_MAXGRP * HB_FDS];
static char __aligned(4) hbbuf[HB_MAXGRP * HB_FDS][8 * sizeof(struct hb_msg)];
static atomic_t hb_errors;

static void hb_sender(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a, g = id / (2 * HB_FDS);
	struct hb_msg m = {.sender = id};

	for (uint32_t seq = 0; seq < HB_LOOPS; seq++) {
		m.seq = seq;
		memset(m.data, (uint8_t)(seq ^ id), sizeof(m.data));
		for (int r = 0; r < HB_FDS; r++) {
			k_msgq_put(&hbq[g * HB_FDS + r], &m, K_FOREVER);
		}
		progress[id]++;
	}
}

static void hb_receiver(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a, g = id / (2 * HB_FDS), r = id % (2 * HB_FDS) - HB_FDS;
	uint32_t next[NTHREADS] = {0};
	struct hb_msg m;

	for (int n = 0; n < HB_LOOPS * HB_FDS; n++) {
		k_msgq_get(&hbq[g * HB_FDS + r], &m, K_FOREVER);

		uint8_t want = (uint8_t)(m.seq ^ m.sender);
		bool ok = m.sender < NTHREADS && m.sender / (2 * HB_FDS) == g &&
			  m.seq == next[m.sender];

		/* Not in hackbench, which never reads the data (see README). */
		for (int k = 0; ok && k < HB_DATA; k++) {
			ok = m.data[k] == want;
		}
		if (!ok) {
			atomic_inc(&hb_errors);
		}
		if (m.sender < NTHREADS) {
			next[m.sender] = m.seq + 1;
		}
		progress[id]++;
	}
}

ZTEST(sched_torture, test_01_hackbench)
{
	ONLY(1);
	int groups = MIN(CONFIG_SCHED_TORTURE_HACKBENCH_GROUPS, HB_MAXGRP);
	int n = groups * 2 * HB_FDS;

	atomic_clear(&hb_errors);
	for (int q = 0; q < groups * HB_FDS; q++) {
		k_msgq_init(&hbq[q], hbbuf[q], sizeof(struct hb_msg), 8);
	}
	monitor_start(n);

	uint32_t t0 = k_uptime_get_32();

	for (int i = 0; i < n; i++) {
		bool sender = i % (2 * HB_FDS) < HB_FDS;

		spawn(i, sender ? hb_sender : hb_receiver, K_PRIO_PREEMPT(5), -1);
	}
	join(0, n);

	uint32_t ms = k_uptime_get_32() - t0;

	stop = true;
	k_thread_join(&mon_thread, K_FOREVER);
	printk("ST hackbench: %d groups, %d senders and %d receivers each, %d loops, %d bytes: "
	       "Time %u.%03u s, data errors %ld\n",
	       groups, HB_FDS, HB_FDS, HB_LOOPS, HB_DATA, ms / 1000, ms % 1000,
	       (long)atomic_get(&hb_errors));
	zassert_equal(atomic_get(&hb_errors), 0, "corrupted, reordered or foreign messages");
	zassert_equal(stalls, 0);
}

/* ---------------------------------------------------------------------------
 * 2. schbench: message threads each wake their workers, which record how
 * long the wakeup took, run for a while and report back; the message thread
 * then sleeps. Latency percentiles as schbench prints them.
 */
static int sb_msg, sb_workers;
static struct k_sem sb_go[NTHREADS], sb_done[NTHREADS];
static volatile uint32_t sb_stamp[NTHREADS];
static volatile int sb_cpu[NTHREADS];
/* Wakeups on the waker's CPU, and wakeups that needed another CPU (IPI). */
static struct lat sb_local[NTHREADS], sb_remote[NTHREADS];

static void sb_message(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	int first = sb_msg + id * sb_workers;

	while (!stop) {
		unsigned int key = arch_irq_lock();
		uint32_t now = k_cycle_get_32();
		int cpu = cur_cpu();

		arch_irq_unlock(key);
		for (int w = first; w < first + sb_workers; w++) {
			sb_stamp[w] = now;
			sb_cpu[w] = cpu;
			k_sem_give(&sb_go[w]);
		}
		for (int w = first; w < first + sb_workers; w++) {
			k_sem_take(&sb_done[w], K_MSEC(STALL_MS));
		}
		k_usleep(CONFIG_SCHED_TORTURE_SCHBENCH_SLEEP_US);
		progress[id]++;
	}
	for (int w = first; w < first + sb_workers; w++) {
		k_sem_give(&sb_go[w]);
	}
}

static void sb_worker(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;

	while (!stop) {
		if (k_sem_take(&sb_go[id], K_MSEC(100)) != 0 || stop) {
			progress[id]++;
			continue;
		}

		unsigned int key = arch_irq_lock();
		uint32_t us = cyc_to_us(k_cycle_get_32() - sb_stamp[id]);
		bool local = cur_cpu() == (uint32_t)sb_cpu[id];

		arch_irq_unlock(key);
		lat_add(local ? &sb_local[id] : &sb_remote[id], us);
		k_busy_wait(CONFIG_SCHED_TORTURE_SCHBENCH_CPUTIME_US);
		k_sem_give(&sb_done[id]);
		progress[id]++;
	}
}

static void sb_print(const char *name, const struct lat *l)
{
	printk("ST schbench %s: n %u p50 <=%u p90 <=%u p99 <=%u p99.9 <=%u min %u max %u us\n",
	       name, l->n, lat_pct(l, 500), lat_pct(l, 900), lat_pct(l, 990), lat_pct(l, 999),
	       l->min_us, l->max_us);
}

ZTEST(sched_torture, test_02_schbench)
{
	ONLY(2);
	struct lat local = {0}, remote = {0}, all = {0};
	int n, nh = IS_ENABLED(CONFIG_SCHED_TORTURE_SCHBENCH_HOGS) ? ncpus() : 0;

	sb_msg = MIN(CONFIG_SCHED_TORTURE_SCHBENCH_MESSAGE_THREADS, (NTHREADS - nh) / 2);
	sb_workers = MAX(1, MIN(ncpus(), (NTHREADS - nh - sb_msg) / sb_msg));
	n = sb_msg + sb_msg * sb_workers;
	memset(sb_local, 0, sizeof(sb_local));
	memset(sb_remote, 0, sizeof(sb_remote));
	for (int w = 0; w < n; w++) {
		k_sem_init(&sb_go[w], 0, 1);
		k_sem_init(&sb_done[w], 0, 1);
	}
	monitor_start(n);
	for (int w = sb_msg; w < n; w++) {
		spawn(w, sb_worker, K_PRIO_PREEMPT(5), -1);
	}
	for (int m = 0; m < sb_msg; m++) {
		spawn(m, sb_message, K_PRIO_PREEMPT(5), -1);
	}
	for (int h = 0; h < nh; h++) {
		spawn(n + h, hog, K_LOWEST_APPLICATION_THREAD_PRIO, h);
	}
	k_msleep(DUR_MS);
	stop = true;
	join(0, n + nh);
	k_thread_join(&mon_thread, K_FOREVER);
	for (int w = sb_msg; w < n; w++) {
		lat_merge(&local, &sb_local[w]);
		lat_merge(&remote, &sb_remote[w]);
	}
	lat_merge(&all, &local);
	lat_merge(&all, &remote);
	printk("ST schbench: %d message threads, %d workers each, %d CPUs, hogs %d\n", sb_msg,
	       sb_workers, ncpus(), nh);
	sb_print("all", &all);
	sb_print("same CPU", &local);
	sb_print("other CPU", &remote);
	zassert_true(all.n > 0);
	zassert_equal(stalls, 0);
}

/* ---------------------------------------------------------------------------
 * 3. cyclictest -S: one thread per CPU at the highest preemptible priority,
 * pinned, sleeping to absolute times; thread i uses interval + i * distance.
 * cyclictest adds no load of its own; CONFIG_SCHED_TORTURE_CYCLICTEST_LOAD
 * adds some, standing in for the load one would run beside it.
 */
static struct lat ct_lat[CONFIG_MP_MAX_NUM_CPUS];
static struct k_msgq ct_q;
static char __aligned(4) ct_qbuf[8 * sizeof(uint32_t)];

static void ct_measure(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	uint64_t hz = sys_clock_hw_cycles_per_sec();
	uint32_t us = CONFIG_SCHED_TORTURE_CYCLICTEST_INTERVAL_US +
		      id * CONFIG_SCHED_TORTURE_CYCLICTEST_DISTANCE_US;
	int64_t period = MAX(1, (int64_t)k_us_to_ticks_near64(us));
	int64_t next = k_uptime_ticks() + 1;

	k_sleep(K_TIMEOUT_ABS_TICKS(next));

	uint32_t c0 = k_cycle_get_32();

	for (uint64_t n = 1; !stop; n++) {
		next += period;
		k_sleep(K_TIMEOUT_ABS_TICKS(next));

		uint32_t want = c0 + (uint32_t)(hz * period * n / CONFIG_SYS_CLOCK_TICKS_PER_SEC);
		int32_t late = (int32_t)(k_cycle_get_32() - want);

		lat_add(&ct_lat[id], late > 0 ? cyc_to_us(late) : 0);
		progress[id]++;
	}
}

static void ct_churn(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	uint32_t v = 0;

	while (!stop) {
		if (id & 1) {
			k_msgq_put(&ct_q, &v, K_MSEC(10));
			v++;
		} else {
			k_msgq_get(&ct_q, &v, K_MSEC(10));
		}
	}
}

ZTEST(sched_torture, test_03_cyclictest)
{
	ONLY(3);
	int nc = ncpus(), i = 0;

	memset(ct_lat, 0, sizeof(ct_lat));
	monitor_start(nc);
	for (int c = 0; c < nc; c++) {
		spawn(i++, ct_measure, K_PRIO_PREEMPT(0), c);
	}
	if (IS_ENABLED(CONFIG_SCHED_TORTURE_CYCLICTEST_LOAD)) {
		k_msgq_init(&ct_q, ct_qbuf, sizeof(uint32_t), 8);
		for (int k = 0; k < 4 && i < NTHREADS; k++) {
			spawn(i++, ct_churn, K_PRIO_PREEMPT(8), -1);
		}
		for (int c = 0; c < nc && i < NTHREADS; c++) {
			spawn(i++, hog, K_LOWEST_APPLICATION_THREAD_PRIO, c);
		}
	}
	k_msleep(DUR_MS);
	stop = true;
	join(0, i);
	k_thread_join(&mon_thread, K_FOREVER);
	for (int c = 0; c < nc; c++) {
		const struct lat *l = &ct_lat[c];

		printk("ST cyclictest T:%d (cpu %d) I:%d C:%u Min:%u Avg:%u Max:%u us%s\n", c, c,
		       CONFIG_SCHED_TORTURE_CYCLICTEST_INTERVAL_US +
			       c * CONFIG_SCHED_TORTURE_CYCLICTEST_DISTANCE_US,
		       l->n, l->min_us, l->n ? (uint32_t)(l->sum_us / l->n) : 0, l->max_us,
		       IS_ENABLED(CONFIG_SCHED_TORTURE_CYCLICTEST_LOAD) ? " (with load)" : "");
		zassert_true(l->n > 0);
	}
	zassert_equal(stalls, 0);
}

/* ---------------------------------------------------------------------------
 * 4. locktorture: for each lock type, 2 * ncpus writers repeatedly acquire
 * the lock, hold it for a short delay (rarely a long one) and release it;
 * two holders at once is a failure. The writers stutter (run, then pause)
 * and, on SMP with CPU masks, are shuffled between CPUs leaving one idle.
 * k_spinlock always masks interrupts, so it maps to "spin_lock_irq".
 */
enum lt_type {
	LT_SPIN_IRQ,
	LT_MUTEX,
	LT_SEM,
	LT_COUNT,
};

static const char *const lt_name[LT_COUNT] = {"spin_lock_irq", "mutex_lock", "sem (binary)"};

static enum lt_type lt_cur;
static struct k_spinlock lt_spin;
static struct k_mutex lt_mutex;
static struct k_sem lt_sem;
static atomic_t lt_held, lt_fail;
static volatile bool lt_paused;
static uint32_t lt_acq[NTHREADS];
static int lt_writers;

/* locktorture's write delays: rarely long, otherwise short */
static void lt_delay(uint32_t *s)
{
	if (lt_cur == LT_SPIN_IRQ) {
		if (rnd(s) % (lt_writers * 2000 * lt_long_hold_ms) == 0) {
			k_busy_wait(lt_long_hold_ms * 1000);
		} else if (rnd(s) % (lt_writers * 2 * 2) == 0) {
			k_busy_wait(2);
		}
	} else {
		if (rnd(s) % (lt_writers * 2000 * 5) == 0) {
			k_busy_wait(5 * 5000);
		} else {
			k_busy_wait(5 * 1000 / 5);
		}
	}
}

static void lt_writer(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	uint32_t s = seed_mix(0x9e3779b9u * (id + 1));

	while (!stop) {
		if (lt_paused) {
			k_msleep(10);
			continue;
		}

		k_spinlock_key_t key = {0};

		switch (lt_cur) {
		case LT_SPIN_IRQ:
			key = k_spin_lock(&lt_spin);
			break;
		case LT_MUTEX:
			k_mutex_lock(&lt_mutex, K_FOREVER);
			break;
		default:
			k_sem_take(&lt_sem, K_FOREVER);
			break;
		}
		if (atomic_inc(&lt_held) != 0) {
			atomic_inc(&lt_fail);
		}
		lt_delay(&s);
		atomic_dec(&lt_held);
		switch (lt_cur) {
		case LT_SPIN_IRQ:
			k_spin_unlock(&lt_spin, key);
			break;
		case LT_MUTEX:
			k_mutex_unlock(&lt_mutex);
			break;
		default:
			k_sem_give(&lt_sem);
			break;
		}
		lt_acq[id]++;
		progress[id]++;
		/* stands in for cond_resched() between acquisitions */
		if ((rnd(&s) & 0xff) == 0) {
			k_yield();
		}
	}
}

static void lt_shuffle(uint32_t *s)
{
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK) && \
	!defined(CONFIG_SCHED_CPU_MASK_PIN_ONLY)
	if (ncpus() < 2) {
		return;
	}
	/* Zephyr only changes the CPU mask of a thread that is not runnable,
	 * so shuffle by suspending, moving and resuming (see README).
	 */
	int idle = rnd(s) % ncpus();

	for (int i = 0; i < lt_writers; i++) {
		int cpu = (idle + 1 + rnd(s) % (ncpus() - 1)) % ncpus();

		k_thread_suspend(&threads[i]);
		k_thread_cpu_mask_clear(&threads[i]);
		k_thread_cpu_mask_enable(&threads[i], cpu);
		k_thread_resume(&threads[i]);
	}
#else
	ARG_UNUSED(s);
#endif
}

ZTEST(sched_torture, test_04_locktorture)
{
	ONLY(4);
	bool failed = false;

	lt_writers = MIN(2 * ncpus(), NTHREADS);
	k_mutex_init(&lt_mutex);
	k_sem_init(&lt_sem, 1, 1);
	for (lt_cur = 0; lt_cur < LT_COUNT; lt_cur++) {
		uint32_t s = seed_mix(0xc0ffee11u), total = 0, mx = 0, mn = UINT32_MAX;
		int64_t now = k_uptime_get(), end = now + DUR_MS;
		int64_t next_stutter = now + lt_stutter_ms;
		int64_t next_shuffle = now + lt_shuffle_ms;

		stop = false;
		lt_paused = false;
		atomic_clear(&lt_held);
		atomic_clear(&lt_fail);
		memset(lt_acq, 0, sizeof(lt_acq));
		monitor_start(lt_writers);
		for (int i = 0; i < lt_writers; i++) {
			spawn(i, lt_writer, K_PRIO_PREEMPT(5), -1);
		}
		while ((now = k_uptime_get()) < end) {
			if (lt_stutter_ms && now >= next_stutter) {
				lt_paused = !lt_paused;
				next_stutter = now + lt_stutter_ms;
			}
			if (lt_shuffle_ms && now >= next_shuffle) {
				lt_shuffle(&s);
				next_shuffle = now + lt_shuffle_ms;
			}
			if (lt_paused) {
				monitor_pause(50);
			} else {
				k_msleep(50);
			}
		}
		lt_paused = false;
		stop_and_join(lt_writers);
		for (int i = 0; i < lt_writers; i++) {
			total += lt_acq[i];
			mx = MAX(mx, lt_acq[i]);
			mn = MIN(mn, lt_acq[i]);
		}
		printk("ST locktorture %s: Writes: Total: %u Max/Min: %u/%u Fail: %ld, stalls %u\n",
		       lt_name[lt_cur], total, mx, mn, (long)atomic_get(&lt_fail), stalls);
		failed |= atomic_get(&lt_fail) != 0 || stalls != 0;
	}
	zassert_false(failed, "a lock was held by two writers, or a writer stalled");
}

/* ---------------------------------------------------------------------------
 * 5. pi_stress: inversion groups of a low, a medium and a high priority
 * thread. Low takes the mutex, then high blocks on it while medium spins
 * until high has finished; without priority inheritance low never runs
 * again and the group deadlocks, which the stall monitor reports.
 */
struct pi_group {
	struct k_mutex m;
	struct k_sem go_h, go_m, done_h, done_m;
	volatile bool high_done;
	volatile bool exit; /* set by low only, so medium and high answer every round low starts */
	uint32_t iters;
};

static struct pi_group pig[NTHREADS / 3];

static void pi_low(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	struct pi_group *g = &pig[id / 3];

	while (!stop) {
		g->high_done = false;
		k_mutex_lock(&g->m, K_FOREVER);
		k_sem_give(&g->go_h); /* high blocks on the mutex, boosting us */
		k_sem_give(&g->go_m); /* medium spins until high is done */
		k_busy_wait(100);
		k_mutex_unlock(&g->m);
		k_sem_take(&g->done_h, K_FOREVER);
		k_sem_take(&g->done_m, K_FOREVER);
		g->iters++;
		progress[id]++;
	}
	g->exit = true;
	g->high_done = true;
	k_sem_give(&g->go_h);
	k_sem_give(&g->go_m);
}

static void pi_mid(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	struct pi_group *g = &pig[id / 3];

	for (;;) {
		k_sem_take(&g->go_m, K_FOREVER);
		if (g->exit) {
			break;
		}
		while (!g->high_done) {
			k_busy_wait(10);
		}
		k_sem_give(&g->done_m);
		progress[id]++;
	}
}

static void pi_high(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	struct pi_group *g = &pig[id / 3];

	for (;;) {
		k_sem_take(&g->go_h, K_FOREVER);
		if (g->exit) {
			break;
		}
		k_mutex_lock(&g->m, K_FOREVER);
		k_mutex_unlock(&g->m);
		g->high_done = true;
		k_sem_give(&g->done_h);
		progress[id]++;
	}
}

ZTEST(sched_torture, test_05_pi_stress)
{
	ONLY(5);
	/* pi_stress defaults to one group fewer than there are CPUs. */
	int ng = MIN(MAX(1, ncpus() - 1), NTHREADS / 3), n = 3 * ng;
	bool up = IS_ENABLED(CONFIG_SCHED_TORTURE_PI_UNIPROCESSOR);
	uint32_t iters = 0;

	for (int i = 0; i < ng; i++) {
		struct pi_group *g = &pig[i];

		memset(g, 0, sizeof(*g));
		k_mutex_init(&g->m);
		k_sem_init(&g->go_h, 0, 1);
		k_sem_init(&g->go_m, 0, 1);
		k_sem_init(&g->done_h, 0, 1);
		k_sem_init(&g->done_m, 0, 1);
	}
	monitor_start(n);
	for (int i = 0; i < ng; i++) {
		/* --uniprocessor puts every thread on CPU 0 */
		spawn(3 * i, pi_low, K_PRIO_PREEMPT(9), up ? 0 : -1);
		spawn(3 * i + 1, pi_mid, K_PRIO_PREEMPT(6), up ? 0 : -1);
		spawn(3 * i + 2, pi_high, K_PRIO_PREEMPT(3), up ? 0 : -1);
	}
	k_msleep(DUR_MS);
	stop_and_join(n);
	for (int i = 0; i < ng; i++) {
		iters += pig[i].iters;
	}
	printk("ST pi_stress: %d groups (%s), %u inversions handled, stalls %u\n", ng,
	       up ? "uniprocessor" : "any CPU", iters, stalls);
	zassert_true(iters > 0);
	zassert_equal(stalls, 0, "an inversion group deadlocked");
}

/* ---------------------------------------------------------------------------
 * 6. stress-ng --fork: instances repeatedly create children that exit at
 * once and reap them.
 */
#define FK_CHILDREN 3

static uint32_t fk_ops[NTHREADS];

static void fk_child(void *a, void *b, void *c)
{
}

static void fk_instance(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	int base = (int)(uintptr_t)b;

	while (!stop) {
		for (int k = 0; k < FK_CHILDREN; k++) {
			int slot = base + k;

			k_thread_create(&threads[slot], stacks[slot], STACK, fk_child, NULL, NULL,
					NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
		}
		for (int k = 0; k < FK_CHILDREN; k++) {
			k_thread_join(&threads[base + k], K_FOREVER);
		}
		fk_ops[id] += FK_CHILDREN;
		progress[id]++;
	}
}

static void count_one(const struct k_thread *t, void *d)
{
	ARG_UNUSED(t);
	(*(int *)d)++;
}

static int count_threads(void)
{
	int n = 0;

	k_thread_foreach(count_one, &n);
	return n;
}

ZTEST(sched_torture, test_06_fork)
{
	ONLY(6);
	int ni = MIN(ncpus(), NTHREADS / (1 + FK_CHILDREN)), base = count_threads();
	uint32_t total = 0;

	memset(fk_ops, 0, sizeof(fk_ops));
	monitor_start(ni);
	for (int i = 0; i < ni; i++) {
		k_thread_create(&threads[i], stacks[i], STACK, fk_instance, (void *)(uintptr_t)i,
				(void *)(uintptr_t)(ni + i * FK_CHILDREN), NULL, K_PRIO_PREEMPT(6),
				0, K_NO_WAIT);
	}
	k_msleep(DUR_MS);
	stop_and_join(ni);
	for (int i = 0; i < ni; i++) {
		total += fk_ops[i];
	}

	int after = count_threads();

	printk("ST fork: %d instances, %u children created and reaped, threads %d -> %d\n", ni,
	       total, base, after);
	zassert_true(total > 0);
	zassert_equal(after, base, "thread list leaked or lost entries");
	zassert_equal(stalls, 0);
}

/* ---------------------------------------------------------------------------
 * 7. stress-ng --kill: instances repeatedly start a child and kill it, while
 * it is running (possibly on another CPU) or blocked.
 */
static uint32_t kl_ops[NTHREADS];

static void kl_child(void *a, void *b, void *c)
{
	uint32_t s = seed_mix((uint32_t)(uintptr_t)a | 1);

	for (;;) {
		if (rnd(&s) & 1) {
			k_busy_wait(50);
		} else {
			k_sleep(K_TICKS(1));
		}
	}
}

static void kl_instance(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a, slot = (int)(uintptr_t)b;
	uint32_t s = seed_mix(0x5bd1e995u * (id + 1));

	while (!stop) {
		k_thread_create(&threads[slot], stacks[slot], STACK, kl_child,
				(void *)(uintptr_t)rnd(&s), NULL, NULL, K_PRIO_PREEMPT(6), 0,
				K_NO_WAIT);
		if (rnd(&s) & 1) {
			k_busy_wait(rnd(&s) % 300);
		} else {
			k_sleep(K_TICKS(1));
		}
		k_thread_abort(&threads[slot]);
		k_thread_join(&threads[slot], K_FOREVER);
		kl_ops[id]++;
		progress[id]++;
	}
}

ZTEST(sched_torture, test_07_kill)
{
	ONLY(7);
	int ni = MIN(ncpus(), NTHREADS / 2), base = count_threads();
	uint32_t total = 0;

	memset(kl_ops, 0, sizeof(kl_ops));
	monitor_start(ni);
	for (int i = 0; i < ni; i++) {
		k_thread_create(&threads[i], stacks[i], STACK, kl_instance, (void *)(uintptr_t)i,
				(void *)(uintptr_t)(ni + i), NULL, K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	}
	k_msleep(DUR_MS);
	stop_and_join(ni);
	for (int i = 0; i < ni; i++) {
		total += kl_ops[i];
	}

	int after = count_threads();

	printk("ST kill: %d instances, %u children killed and reaped, threads %d -> %d\n", ni,
	       total, base, after);
	zassert_true(total > 0);
	zassert_equal(after, base, "thread list leaked or lost entries");
	zassert_equal(stalls, 0);
}

/* ---------------------------------------------------------------------------
 * 8. stress-ng --affinity: keep moving threads between CPUs and check that
 * a pinned thread is never seen running elsewhere. stress-ng changes its
 * own affinity; Zephyr cannot change a runnable thread's mask, so a
 * controller suspends, moves and resumes it (see README).
 */
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK) && \
	!defined(CONFIG_SCHED_CPU_MASK_PIN_ONLY)
#define MG_TEST 1

static volatile int mg_expect[NTHREADS];
static volatile uint32_t mg_gen[NTHREADS];
static atomic_t mg_viol;

static void mg_worker(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;

	while (!stop) {
		unsigned int key = arch_irq_lock();
		uint32_t g = mg_gen[id];
		int e = mg_expect[id];
		int cpu = cur_cpu();

		arch_irq_unlock(key);
		if (e >= 0 && g == mg_gen[id] && cpu != e) {
			atomic_inc(&mg_viol);
		}
		progress[id]++;
		k_yield();
	}
}
#endif

ZTEST(sched_torture, test_08_affinity)
{
	ONLY(8);
#ifdef MG_TEST
	uint32_t s = seed_mix(0xc0ffeeu), moves = 0;
	int nc = ncpus(), nw = MIN(nc, NTHREADS);

	atomic_clear(&mg_viol);
	monitor_start(nw);
	for (int i = 0; i < nw; i++) {
		mg_expect[i] = -1;
		mg_gen[i] = 0;
		spawn(i, mg_worker, K_PRIO_PREEMPT(7), -1);
	}
	for (int64_t end = k_uptime_get() + DUR_MS; k_uptime_get() < end;) {
		int i = rnd(&s) % nw, cpu = rnd(&s) % nc;

		k_thread_suspend(&threads[i]);
		k_thread_cpu_mask_clear(&threads[i]);
		k_thread_cpu_mask_enable(&threads[i], cpu);
		mg_expect[i] = cpu;
		mg_gen[i]++;
		k_thread_resume(&threads[i]);
		moves++;
		k_yield();
		if ((moves & 7) == 0) {
			k_sleep(K_TICKS(1));
		}
	}
	stop_and_join(nw);
	printk("ST affinity: %d instances on %d CPUs, %u moves, violations %ld\n", nw, nc, moves,
	       (long)atomic_get(&mg_viol));
	zassert_equal(atomic_get(&mg_viol), 0, "a pinned thread ran on another CPU");
	zassert_equal(stalls, 0);
#else
	ztest_test_skip();
#endif
}

/* ---------------------------------------------------------------------------
 * 9. SCHED_RR fairness: two equal-priority busy threads share a CPU while a
 * higher-priority thread wakes every millisecond. Under SCHED_RR a
 * preempted thread keeps the rest of its slice, so the two split the CPU.
 * Zephyr restarts the slice of whichever thread it switches to, so the one
 * that keeps getting preempted may never give the CPU to its peer.
 */
static volatile uint32_t fa_runs[2];

static void fa_busy(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;

	while (!stop) {
		fa_runs[id]++;
		k_busy_wait(10);
	}
}

static void fa_tick(void *a, void *b, void *c)
{
	while (!stop) {
		k_sleep(K_MSEC(1));
	}
}

ZTEST(sched_torture, test_09_rr_fairness)
{
	ONLY(9);
	uint32_t a, b, lo, hi;

	fa_runs[0] = fa_runs[1] = 0;
	spawn(0, fa_busy, K_PRIO_PREEMPT(6), 0);
	spawn(1, fa_busy, K_PRIO_PREEMPT(6), 0);
	spawn(2, fa_tick, K_PRIO_PREEMPT(2), 0);
	k_msleep(DUR_MS);
	stop = true;
	join(0, 3);
	a = fa_runs[0];
	b = fa_runs[1];
	lo = MIN(a, b);
	hi = MAX(a, b);
	printk("ST rr fairness: equal-priority shares %u / %u (lesser %u%%), slice %d ms\n", a, b,
	       a + b ? (uint32_t)((uint64_t)lo * 100 / (a + b)) : 0, CONFIG_TIMESLICE_SIZE);
	zassert_true(lo * 4 >= hi, "an equal-priority thread got less than a fifth of the CPU");
}

/* ---------------------------------------------------------------------------
 * 10. mixed operations (no Linux counterpart): a random mix of mutexes, a
 * counting semaphore, a spinlock also taken from a timer ISR, yields,
 * sleeps and priority changes; mutual exclusion and conservation checked.
 */
#define MX_MUTEXES 3
#define MX_POOL    3

static struct k_mutex mx_mtx[MX_MUTEXES];
static k_tid_t volatile mx_owner[MX_MUTEXES];
static struct k_sem mx_pool;
static atomic_t mx_held, mx_viol, mx_sl_ops;
static struct k_spinlock mx_sl;
static uint32_t mx_sl_count;
static uint32_t mx_ops[NTHREADS];

static void mx_isr(struct k_timer *t)
{
	k_spinlock_key_t key = k_spin_lock(&mx_sl);

	mx_sl_count++;
	k_spin_unlock(&mx_sl, key);
	atomic_inc(&mx_sl_ops);
}
K_TIMER_DEFINE(mx_timer, mx_isr, NULL);

static void mx_section(int m)
{
	k_tid_t self = k_current_get();

	if (mx_owner[m] != NULL) {
		atomic_inc(&mx_viol);
	}
	mx_owner[m] = self;
	k_busy_wait(5);
	if (mx_owner[m] != self) {
		atomic_inc(&mx_viol);
	}
	mx_owner[m] = NULL;
}

static void mx_worker(void *a, void *b, void *c)
{
	int id = (int)(uintptr_t)a;
	uint32_t s = seed_mix(0x9e3779b9u * (id + 1));

	while (!stop) {
		uint32_t op = rnd(&s) % 100;

		if (op < 30) {
			int m = rnd(&s) % MX_MUTEXES;

			k_mutex_lock(&mx_mtx[m], K_FOREVER);
			mx_section(m);
			k_mutex_unlock(&mx_mtx[m]);
		} else if (op < 40) {
			k_mutex_lock(&mx_mtx[0], K_FOREVER);
			k_mutex_lock(&mx_mtx[1], K_FOREVER);
			mx_section(0);
			mx_section(1);
			k_mutex_unlock(&mx_mtx[1]);
			k_mutex_unlock(&mx_mtx[0]);
		} else if (op < 52) {
			if (k_sem_take(&mx_pool, K_MSEC(5)) == 0) {
				if (atomic_inc(&mx_held) + 1 > MX_POOL) {
					atomic_inc(&mx_viol);
				}
				k_busy_wait(10);
				atomic_dec(&mx_held);
				k_sem_give(&mx_pool);
			}
		} else if (op < 64) {
			k_spinlock_key_t key = k_spin_lock(&mx_sl);

			mx_sl_count++;
			k_spin_unlock(&mx_sl, key);
			atomic_inc(&mx_sl_ops);
		} else if (op < 72) {
			k_yield();
		} else if (op < 82) {
			k_sleep(K_TICKS(1 + rnd(&s) % 3));
		} else if (op < 90) {
			k_thread_priority_set(k_current_get(), K_PRIO_PREEMPT(3 + rnd(&s) % 7));
		} else {
			k_busy_wait(rnd(&s) % 200);
		}
		mx_ops[id]++;
		progress[id]++;
	}
}

ZTEST(sched_torture, test_10_mixed_ops)
{
	ONLY(10);
	int nw = MIN(4 * ncpus() + 2, NTHREADS);
	uint32_t total = 0;

	for (int m = 0; m < MX_MUTEXES; m++) {
		k_mutex_init(&mx_mtx[m]);
		mx_owner[m] = NULL;
	}
	k_sem_init(&mx_pool, MX_POOL, MX_POOL);
	atomic_clear(&mx_held);
	atomic_clear(&mx_viol);
	atomic_clear(&mx_sl_ops);
	mx_sl_count = 0;
	memset(mx_ops, 0, sizeof(mx_ops));
	monitor_start(nw);
	k_timer_start(&mx_timer, K_MSEC(1), K_MSEC(1));
	for (int i = 0; i < nw; i++) {
		/* One starting priority: the mix changes priorities itself. */
		spawn(i, mx_worker, K_PRIO_PREEMPT(6), -1);
	}
	k_msleep(DUR_MS);
	stop_and_join(nw);
	k_timer_stop(&mx_timer);
	for (int i = 0; i < nw; i++) {
		total += mx_ops[i];
	}
	printk("ST mixed: %d workers, %u ops, violations %ld, spinlock %u/%ld, stalls %u\n", nw,
	       total, (long)atomic_get(&mx_viol), mx_sl_count, (long)atomic_get(&mx_sl_ops),
	       stalls);
	zassert_equal(atomic_get(&mx_viol), 0, "mutual exclusion or semaphore count violated");
	zassert_equal(mx_sl_count, (uint32_t)atomic_get(&mx_sl_ops), "spinlock lost updates");
}

ZTEST_SUITE(sched_torture, NULL, seed_setup, before, NULL, NULL);
