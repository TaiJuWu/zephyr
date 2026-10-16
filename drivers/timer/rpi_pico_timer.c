/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief System timer on the RP2350 TIMER peripheral
 *
 * A 64-bit 1 MHz counter shared by both cores, with 32-bit alarms that fire
 * when the low word of the counter *equals* the alarm (COMPARE_EXACT). Unlike
 * the per-core SysTick, one shared comparator serves every CPU: any CPU may
 * arm it under the generic core's lock, the interrupt is taken on the CPU that
 * enabled it (CPU 0), and timeouts for other CPUs are delivered by the kernel
 * with an IPI. smp_timer_init() therefore has nothing to do.
 */

#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <hardware/structs/timer.h>

#define TIMER_NODE DT_NODELABEL(timer0)
#define TIMER_HW   ((timer_hw_t *)DT_REG_ADDR(TIMER_NODE))
#define TIMER_IRQN DT_IRQ_BY_IDX(TIMER_NODE, 0, irq)
#define TIMER_PRIO DT_IRQ_BY_IDX(TIMER_NODE, 0, priority)
#define ALARM_NUM  0

BUILD_ASSERT(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC == 1000000,
	     "the RP2350 TIMER counts at 1 MHz");

#define TIMER_CORE_BACKEND_COMPARE_EXACT
#define TIMER_CORE_COUNTER_WIDTH    64
#define TIMER_CORE_ALARM_MAX_CYCLES UINT32_MAX

static inline uint64_t timer_driver_cycle_get(void)
{
	uint32_t hi, lo;

	/* The raw registers are not latched: re-read across a carry. */
	do {
		hi = TIMER_HW->timerawh;
		lo = TIMER_HW->timerawl;
	} while (hi != TIMER_HW->timerawh);

	return ((uint64_t)hi << 32) | lo;
}

static inline void timer_driver_set_compare(uint64_t cycles)
{
	/* Writing the alarm arms it; it matches the counter's low word. */
	TIMER_HW->alarm[ALARM_NUM] = (uint32_t)cycles;
}

#include "system_timer_generic.h"

static void timer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	TIMER_HW->intr = BIT(ALARM_NUM);
	timer_core_announce();
}

static int sys_clock_driver_init(void)
{
	IRQ_CONNECT(TIMER_IRQN, TIMER_PRIO, timer_isr, NULL, 0);
	TIMER_HW->intr = BIT(ALARM_NUM);
	TIMER_HW->inte |= BIT(ALARM_NUM);
	timer_core_init();
	irq_enable(TIMER_IRQN);
	return 0;
}

#ifdef CONFIG_SMP
void smp_timer_init(void)
{
	/* Shared comparator, serviced on CPU 0: nothing to do per CPU. */
}
#endif

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
