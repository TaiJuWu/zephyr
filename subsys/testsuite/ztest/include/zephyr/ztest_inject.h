/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Injection points for timing-sensitive kernel tests
 *
 * Kernel code marks spots where a race window opens with an injection
 * point. A test attaches a handler to a point and from there can hold the
 * CPU that reached it, or run a function in interrupt context on the other
 * CPUs while it is held, so that a window that otherwise opens for a few
 * instructions can be entered on purpose.
 *
 * Handlers run in the context of the code that reached the point, which can
 * be an ISR, with spinlocks held and interrupts masked. They must not block.
 */

#ifndef ZEPHYR_TESTSUITE_ZTEST_INJECT_H_
#define ZEPHYR_TESTSUITE_ZTEST_INJECT_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Injection points
 *
 * One entry per point: Z_INJECT_POINT(name, argument description).
 *
 * - TIMEOUT_INFLIGHT: the announce loop has taken a due timeout off the
 *   queue and marked it in flight, and its handler has not run yet. No lock
 *   is held. Argument: the struct _timeout.
 * - HALT_WAIT: halt_thread() found the timeout of a thread it has already
 *   marked dead in flight on another CPU and is about to wait for it, with
 *   the scheduler lock held and interrupts masked. Argument: the thread.
 * - SCHED_IPI: entry of z_sched_ipi(), in the IPI handler, before it takes
 *   any lock. Argument: NULL. Used by ztest_inject_ipi_arm() while an IPI
 *   injection is armed.
 */
#define ZTEST_INJECT_POINT_LIST(Z_INJECT_POINT)                                                    \
	Z_INJECT_POINT(TIMEOUT_INFLIGHT, "struct _timeout *")                                      \
	Z_INJECT_POINT(HALT_WAIT, "struct k_thread *")                                             \
	Z_INJECT_POINT(SCHED_IPI, "NULL")

/** @cond INTERNAL_HIDDEN */
#define Z_INJECT_POINT_ENUM(name, arg) ZTEST_INJECT_##name,
/** @endcond */

/** Injection point identifiers, ZTEST_INJECT_<name> */
enum ztest_inject_point {
	ZTEST_INJECT_POINT_LIST(Z_INJECT_POINT_ENUM)
	/** Number of points */
	ZTEST_INJECT_POINT_COUNT,
};

/**
 * @brief Handler attached to an injection point
 *
 * @param point Point that was reached
 * @param arg Argument passed by the point
 * @param user_data Pointer given to ztest_inject_set()
 */
typedef void (*ztest_inject_handler_t)(enum ztest_inject_point point, void *arg,
				       void *user_data);

/**
 * @brief Attach a handler to an injection point
 *
 * Replaces any handler already attached. The handler can run on any CPU
 * from the moment this returns.
 */
void ztest_inject_set(enum ztest_inject_point point, ztest_inject_handler_t handler,
		      void *user_data);

/**
 * @brief Detach the handler of an injection point
 *
 * Returns once no CPU is running the handler any more.
 */
void ztest_inject_clear(enum ztest_inject_point point);

/** @cond INTERNAL_HIDDEN */
void z_ztest_inject(enum ztest_inject_point point, void *arg);
/** @endcond */

/**
 * @brief Hold the first CPU that reaches a point
 *
 * Armed with ztest_inject_park_arm(). The first CPU that reaches the point
 * with a matching argument spins there until ztest_inject_park_release(),
 * or until CONFIG_ZTEST_INJECT_SPIN_TIMEOUT_MS passes, which is recorded in
 * @c timed_out.
 *
 * The thread that waits for the park and releases it must run on another
 * CPU than the parked one, since that CPU makes no progress while parked.
 */
struct ztest_inject_park {
	/** @cond INTERNAL_HIDDEN */
	const void *match;
	atomic_t armed;
	atomic_t parked;
	atomic_t release;
	/** @endcond */
	/** CPU that parked, valid once ztest_inject_park_wait() succeeded */
	int cpu;
	/** Argument of the point that parked */
	void *arg;
	/** Set if the parked CPU gave up waiting for the release */
	bool timed_out;
};

/**
 * @brief Arm a park on a point
 *
 * @param pk Park descriptor, must stay valid until disarmed
 * @param point Point to park at
 * @param match Park only when the point's argument equals this, or any
 *              argument if NULL
 */
void ztest_inject_park_arm(struct ztest_inject_park *pk, enum ztest_inject_point point,
			   const void *match);

/**
 * @brief Wait until a CPU is parked
 *
 * Polls without sleeping, so the caller keeps its CPU.
 *
 * @retval true a CPU is parked
 * @retval false nothing parked within @p timeout_ms
 */
bool ztest_inject_park_wait(struct ztest_inject_park *pk, uint32_t timeout_ms);

/** @brief Let the parked CPU continue, and detach the park from its point */
void ztest_inject_park_release(struct ztest_inject_park *pk, enum ztest_inject_point point);

#if defined(CONFIG_SCHED_IPI_SUPPORTED) || defined(__DOXYGEN__)
/**
 * @brief Run a function in interrupt context on the other CPUs at a point
 *
 * Armed with ztest_inject_ipi_arm(). The first CPU that reaches the point
 * with a matching argument sends an IPI to every other CPU, which runs
 * @c func at the start of its IPI handler, before z_sched_ipi() takes any
 * lock, and spins until all of them have returned (bounded by
 * CONFIG_ZTEST_INJECT_SPIN_TIMEOUT_MS).
 *
 * @c func must not need anything the CPU at the point holds, for example the
 * scheduler lock at HALT_WAIT. Only one IPI injection can be armed at a
 * time, since it takes over the SCHED_IPI point.
 */
struct ztest_inject_ipi {
	/** @cond INTERNAL_HIDDEN */
	const void *match;
	atomic_t armed;
	atomic_t todo;
	atomic_t pending;
	/** @endcond */
	/** Function to run on each other CPU, with the point's argument */
	void (*func)(struct ztest_inject_ipi *ii, void *arg);
	/** Argument of the point that fired */
	void *arg;
	/** CPU that reached the point */
	int src_cpu;
	/** Set once the point fired */
	bool fired;
	/** Set if the source CPU gave up waiting for the other CPUs */
	bool timed_out;
};

/**
 * @brief Arm an IPI injection on a point
 *
 * @param ii Descriptor with @c func set, must stay valid until disarmed
 * @param point Point to fire at, not SCHED_IPI
 * @param match Fire only when the point's argument equals this, or any
 *              argument if NULL
 */
void ztest_inject_ipi_arm(struct ztest_inject_ipi *ii, enum ztest_inject_point point,
			  const void *match);

/** @brief Detach an IPI injection from its point and from SCHED_IPI */
void ztest_inject_ipi_disarm(struct ztest_inject_ipi *ii, enum ztest_inject_point point);
#endif /* CONFIG_SCHED_IPI_SUPPORTED */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_TESTSUITE_ZTEST_INJECT_H_ */
