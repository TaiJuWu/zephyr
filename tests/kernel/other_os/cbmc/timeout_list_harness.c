/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CBMC proof harness for the delta-list timeout backend
 * (kernel/timeout/list.h), in the style of FreeRTOS/Test/CBMC/proofs:
 * the real header is compiled on its own against minimal definitions, and
 * a bounded, nondeterministic sequence of insert, remove and announce
 * steps is checked against an absolute-time model.
 *
 * Checked after every step, for every interleaving within the bounds:
 * - each queued timeout's z_timeout_q_remainder() equals its absolute
 *   expiry minus the current tick;
 * - z_timeout_q_insert() and z_timeout_q_remove() report "was/is first"
 *   correctly;
 * - the announce loop (as in sys_clock_announce()) pops exactly the
 *   timeouts due at each tick, never one early or late;
 * - no out-of-bounds or invalid pointer access, no signed overflow.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef int64_t k_ticks_t;
#define K_TICKS_FOREVER ((k_ticks_t)(-1))
#define CONTAINER_OF(ptr, type, field) ((type *)(((char *)(ptr)) - offsetof(type, field)))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#include <zephyr/sys/dlist.h>

struct _timeout {
	sys_dnode_t node;
	k_ticks_t dticks;
	void (*fn)(struct _timeout *t);
};

#include "kernel/timeout/list.h"

#ifndef N
#define N 3
#endif
#ifndef STEPS
#define STEPS 4
#endif
#ifndef MAXD
#define MAXD 1000
#endif

int nondet_int(void);
int64_t nondet_i64(void);

static struct _timeout to[N];
static bool linked[N];
static int64_t abs_exp[N];
static int64_t now;

static int idx_of(struct _timeout *t)
{
	for (int i = 0; i < N; i++) {
		if (t == &to[i]) {
			return i;
		}
	}
	__CPROVER_assert(0, "unknown timeout in the queue");
	return -1;
}

static void check(void)
{
	int count = 0;

	for (struct _timeout *t = z_timeout_q_first(); t != NULL; t = z_timeout_q_next(t)) {
		__CPROVER_assert(t->dticks >= 0, "negative delta");
		__CPROVER_assert(linked[idx_of(t)], "queued but not linked in the model");
		count++;
	}
	for (int i = 0; i < N; i++) {
		if (linked[i]) {
			count--;
			__CPROVER_assert(z_timeout_q_remainder(&to[i]) == abs_exp[i] - now,
					 "remainder differs from absolute expiry");
		}
	}
	__CPROVER_assert(count == 0, "queue and model disagree on what is queued");
}

/* Is i at the head of the queue? */
static bool is_head(int i)
{
	return z_timeout_q_first() == &to[i];
}

int main(void)
{
	for (int i = 0; i < N; i++) {
		sys_dnode_init(&to[i].node);
		linked[i] = false;
	}
	now = 0;

	for (int step = 0; step < STEPS; step++) {
		int op = nondet_int();
		int i = nondet_int();

		__CPROVER_assume((i >= 0) && (i < N));

		if (op == 0) {
			int64_t d = nondet_i64();

			__CPROVER_assume(!linked[i]);
			__CPROVER_assume((d >= 0) && (d <= MAXD));
			bool expect_first = true;
			bool first;

			abs_exp[i] = now + d;
			/* Inserted after any timeout with the same expiry. */
			for (int j = 0; j < N; j++) {
				if ((j != i) && linked[j] && (abs_exp[j] <= abs_exp[i])) {
					expect_first = false;
				}
			}
			first = z_timeout_q_insert(&to[i], d);
			linked[i] = true;
			__CPROVER_assert(first == expect_first, "insert misreports first");
		} else if (op == 1) {
			__CPROVER_assume(linked[i]);
			bool was_head = is_head(i);
			bool first = z_timeout_q_remove(&to[i]);

			linked[i] = false;
			__CPROVER_assert(first == was_head, "remove misreports first");
		} else {
			int64_t ticks = nondet_i64();
			int64_t remaining;

			__CPROVER_assume((ticks >= 0) && (ticks <= MAXD));
			remaining = ticks;
			/* The loop of sys_clock_announce() with the DLIST backend */
			while (remaining > 0) {
				int32_t dt = z_timeout_q_next_gap();
				struct _timeout *t;

				if ((uint32_t)dt > remaining) {
					dt = (int32_t)remaining;
				}
				z_timeout_q_advance(dt);
				now += dt;
				remaining -= dt;
				while ((t = z_timeout_q_pop_due()) != NULL) {
					int k = idx_of(t);

					__CPROVER_assert(abs_exp[k] == now, "popped at the wrong tick");
					linked[k] = false;
				}
			}
			for (int k = 0; k < N; k++) {
				__CPROVER_assert(!linked[k] || (abs_exp[k] > now) ||
						 ((abs_exp[k] == now) && (ticks == 0)),
						 "a due timeout was left queued");
			}
		}
		check();
	}
	return 0;
}
