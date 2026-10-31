/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * k_event torture: threads on every CPU and a timer ISR post, wait and
 * clear on one event object at once, with very short timeouts so that
 * wakeups race with expiring timeouts.
 *
 * Bits 0-15 are tokens: a producer posts bit i only when it is not
 * outstanding, and only _safe waiters take them. One post hands a bit to
 * every waiter it satisfies (they all see the same events, the consumed
 * ones are cleared afterwards), so each token bit has a single consumer:
 * consumer c owns the bits i with i % OWNERS == c, the ISR the last group.
 * Checks:
 * - a token is never taken twice (taking one that is not outstanding);
 * - nothing is lost: when everything has stopped, the token bits still in
 *   the event are exactly the outstanding ones (a waiter that consumed a
 *   bit but reported a timeout would leave it outstanding but cleared);
 * - a waiter only returns bits it asked for, all of them for wait_all.
 * Bits 16-31 are noise: set_masked() and clear() on them only, which must
 * never change a token bit.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#define TOKENS        16
#define TOKEN_MASK    BIT_MASK(TOKENS)
#define NOISE_MASK    (~(uint32_t)TOKEN_MASK)
#define N_PRODUCERS   3
#define N_CONSUMERS   6
#define OWNERS        (N_CONSUMERS + 1)   /* the ISR is the last owner */
#define N_PLAIN       2
#define N_THREADS     (N_PRODUCERS + N_CONSUMERS + N_PLAIN + 1)
#define STACK_SIZE    (1024 + CONFIG_TEST_EXTRA_STACK_SIZE)

BUILD_ASSERT(TOKENS >= 2 * OWNERS, "every owner needs two bits");

static struct k_event ev;
static atomic_t outstanding;
static atomic_t produced[TOKENS], consumed[TOKENS];
static atomic_t n_timeouts, n_nowait_miss, n_plain_wakes, n_noise;
static volatile bool stop;
static volatile int error_line;
static volatile uint32_t error_val;
static uint32_t seed;

static K_THREAD_STACK_ARRAY_DEFINE(stacks, N_THREADS, STACK_SIZE);
static struct k_thread threads[N_THREADS];

#define FAIL(v)                                                              \
	do {                                                                 \
		if (error_line == 0) {                                       \
			error_val = (v);                                     \
			error_line = __LINE__;                               \
		}                                                            \
		stop = true;                                                 \
	} while (false)

static uint32_t rnd(uint32_t *s)
{
	/* xorshift32 */
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

static k_timeout_t rnd_timeout(uint32_t *s)
{
	switch (rnd(s) % 6) {
	case 0:
		return K_NO_WAIT;
	case 1:
	case 2:
		return K_TICKS(1);
	case 3:
		return K_TICKS(2);
	default:
		return K_TICKS(1 + rnd(s) % 8);
	}
}

static void produce(uint32_t *s)
{
	int i = rnd(s) % TOKENS;

	if (!atomic_test_and_set_bit(&outstanding, i)) {
		atomic_inc(&produced[i]);
		k_event_post(&ev, BIT(i));
	}
}

static uint32_t owned(int owner)
{
	uint32_t m = 0;

	for (int i = owner; i < TOKENS; i += OWNERS) {
		m |= BIT(i);
	}
	return m;
}

/* A random non-empty subset of the bits in own */
static uint32_t rnd_subset(uint32_t *s, uint32_t own)
{
	uint32_t m = rnd(s) & own;

	return (m != 0U) ? m : own;
}

/* Two distinct bits of own (which has at least two) */
static uint32_t rnd_pair(uint32_t *s, uint32_t own)
{
	int idx[TOKENS], n = 0;

	for (int i = 0; i < TOKENS; i++) {
		if ((own & BIT(i)) != 0U) {
			idx[n++] = i;
		}
	}
	int a = rnd(s) % n;
	int b = (a + 1 + rnd(s) % (n - 1)) % n;

	return BIT(idx[a]) | BIT(idx[b]);
}

static void take(uint32_t got)
{
	for (int i = 0; i < TOKENS; i++) {
		if ((got & BIT(i)) == 0U) {
			continue;
		}
		if (!atomic_test_and_clear_bit(&outstanding, i)) {
			FAIL(got);      /* taken twice, or never posted */
		}
		atomic_inc(&consumed[i]);
	}
}

static void producer(void *p1, void *p2, void *p3)
{
	uint32_t s = seed ^ (POINTER_TO_UINT(p1) * 0x9E3779B9U);

	while (!stop) {
		produce(&s);
		if ((rnd(&s) % 4) == 0) {
			k_yield();
		}
		if ((rnd(&s) % 64) == 0) {
			k_sleep(K_TICKS(rnd(&s) % 3));
		}
	}
}

static void consumer(void *p1, void *p2, void *p3)
{
	int c = POINTER_TO_INT(p2);
	uint32_t s = seed ^ (POINTER_TO_UINT(p1) * 0x85EBCA6BU);
	uint32_t own = owned(c);
	bool all = (c % 2) != 0;

	while (!stop) {
		uint32_t mask, got;
		k_timeout_t t = rnd_timeout(&s);

		if (all) {
			mask = rnd_pair(&s, own);
			got = k_event_wait_all_safe(&ev, mask, false, t);
			if ((got != 0U) && (got != mask)) {
				FAIL(got);
			}
		} else {
			mask = rnd_subset(&s, own);
			got = k_event_wait_safe(&ev, mask, false, t);
			if ((got & ~mask) != 0U) {
				FAIL(got);
			}
		}

		if (got != 0U) {
			take(got);
		} else if (K_TIMEOUT_EQ(t, K_NO_WAIT)) {
			atomic_inc(&n_nowait_miss);
		} else {
			atomic_inc(&n_timeouts);
		}
	}
}

/* Waits without consuming: on tokens and noise bits */
static void plain_waiter(void *p1, void *p2, void *p3)
{
	uint32_t s = seed ^ (POINTER_TO_UINT(p1) * 0xC2B2AE35U);

	while (!stop) {
		uint32_t mask = rnd(&s) | BIT(rnd(&s) % 32);
		uint32_t got = k_event_wait(&ev, mask, false, rnd_timeout(&s));

		if ((got & ~mask) != 0U) {
			FAIL(got);
		}
		if (got != 0U) {
			atomic_inc(&n_plain_wakes);
			/* Bits are often already set: don't spin */
			k_yield();
		}
	}
}

static void noise(uint32_t *s)
{
	if ((rnd(s) % 2) == 0) {
		k_event_set_masked(&ev, rnd(s), NOISE_MASK);
	} else {
		k_event_clear(&ev, rnd(s) & NOISE_MASK);
	}
	atomic_inc(&n_noise);
}

static void noise_thread(void *p1, void *p2, void *p3)
{
	uint32_t s = seed ^ 0x27D4EB2FU;

	while (!stop) {
		noise(&s);
		k_yield();
	}
}

static uint32_t isr_seed;

static void isr_fn(struct k_timer *timer)
{
	if (stop) {
		return;
	}
	produce(&isr_seed);
	noise(&isr_seed);
	/* A consuming no-wait from the ISR as well */
	uint32_t got = k_event_wait_safe(&ev, rnd_subset(&isr_seed, owned(OWNERS - 1)), false,
					 K_NO_WAIT);

	if (got != 0U) {
		take(got);
	}
}

static K_TIMER_DEFINE(isr_timer, isr_fn, NULL);

ZTEST(event_torture, test_torture)
{
	int n = 0;

	seed = CONFIG_EVENT_TORTURE_SEED ? CONFIG_EVENT_TORTURE_SEED : (k_cycle_get_32() | 1U);
	isr_seed = seed ^ 0x165667B1U;
	printk("event_torture: seed 0x%08x, %d CPUs, %d s\n", seed,
	       arch_num_cpus(), CONFIG_EVENT_TORTURE_DURATION_S);

	k_event_init(&ev);
	atomic_clear(&outstanding);
	stop = false;
	error_line = 0;

	/* Preemptible, around the same priorities, so that they interleave */
	for (int i = 0; i < N_PRODUCERS; i++, n++) {
		k_thread_create(&threads[n], stacks[n], STACK_SIZE, producer, UINT_TO_POINTER(n),
				NULL, NULL, K_PRIO_PREEMPT(5 + (i % 2)), 0, K_NO_WAIT);
	}
	for (int i = 0; i < N_CONSUMERS; i++, n++) {
		k_thread_create(&threads[n], stacks[n], STACK_SIZE, consumer, UINT_TO_POINTER(n),
				INT_TO_POINTER(i), NULL, K_PRIO_PREEMPT(4 + (i % 3)), 0, K_NO_WAIT);
	}
	for (int i = 0; i < N_PLAIN; i++, n++) {
		k_thread_create(&threads[n], stacks[n], STACK_SIZE, plain_waiter,
				UINT_TO_POINTER(n), NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	}
	k_thread_create(&threads[n], stacks[n], STACK_SIZE, noise_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(6), 0, K_NO_WAIT);
	n++;

	k_timer_start(&isr_timer, K_TICKS(1), K_TICKS(1));

	for (int t = 0; (t < CONFIG_EVENT_TORTURE_DURATION_S) && !stop; t++) {
		k_sleep(K_SECONDS(1));
	}
	stop = true;
	k_timer_stop(&isr_timer);

	/* Every wait is bounded (at most 8 ticks): all threads end */
	for (int i = 0; i < n; i++) {
		zassert_ok(k_thread_join(&threads[i], K_SECONDS(5)), "thread %d hung", i);
	}

	uint32_t left = k_event_test(&ev, TOKEN_MASK);
	uint32_t out = (uint32_t)atomic_get(&outstanding) & TOKEN_MASK;
	long total_p = 0, total_c = 0;

	for (int i = 0; i < TOKENS; i++) {
		long p = atomic_get(&produced[i]), c = atomic_get(&consumed[i]);

		total_p += p;
		total_c += c;
		if ((p - c) != ((out >> i) & 1)) {
			printk("bit %d: produced %ld, consumed %ld, outstanding %u\n", i, p, c,
			       (out >> i) & 1);
		}
	}
	printk("event_torture: produced %ld, consumed %ld, timeouts %ld, no-wait misses %ld, "
	       "plain wakes %ld, noise ops %ld\n", total_p, total_c, atomic_get(&n_timeouts),
	       atomic_get(&n_nowait_miss), atomic_get(&n_plain_wakes), atomic_get(&n_noise));

	zassert_equal(error_line, 0, "check failed at line %d (0x%08x), seed 0x%08x",
		      error_line, error_val, seed);
	zassert_equal(left, out, "tokens in event 0x%04x, outstanding 0x%04x (seed 0x%08x)",
		      left, out, seed);
	zassert_true(total_c > 0, "nothing consumed");
}

ZTEST_SUITE(event_torture, NULL, NULL, NULL, NULL, NULL);
