/*
 * Copyright (c) 2015 Intel corporation
 * Copyright 2025 Arm Limited and/or its affiliates <open-source-office@arm.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file Software interrupts utility code - ARM implementation
 */

#include <zephyr/kernel.h>
#include <zephyr/irq_offload.h>
#include <cmsis_core.h>

#ifdef CONFIG_SMP
/* Per CPU: several CPUs may offload at the same time. The caller holds the
 * scheduler lock, so it stays on its CPU until the SVC is taken.
 */
static volatile irq_offload_routine_t offload_routine[CONFIG_MP_MAX_NUM_CPUS];
static const void *offload_param[CONFIG_MP_MAX_NUM_CPUS];
#define OFFLOAD_CPU arch_curr_cpu()->id
#else
static volatile irq_offload_routine_t offload_routine[1];
static const void *offload_param[1];
#define OFFLOAD_CPU 0
#endif

/* Called by z_arm_svc */
void z_irq_do_offload(void)
{
	unsigned int cpu = OFFLOAD_CPU;

	offload_routine[cpu](offload_param[cpu]);
}

void arch_irq_offload(irq_offload_routine_t routine, const void *parameter)
{
#if defined(CONFIG_ARMV6_M_ARMV8_M_BASELINE) && !defined(CONFIG_ARMV8_M_BASELINE) \
	&& defined(CONFIG_ASSERT)
	/* ARMv6-M HardFault if you make a SVC call with interrupts locked.
	 */
	__ASSERT(__get_PRIMASK() == 0U, "irq_offload called with interrupts locked\n");
#endif /* CONFIG_ARMV6_M_ARMV8_M_BASELINE && CONFIG_ASSERT */

#ifdef CONFIG_IRQ_OFFLOAD_NESTED
	/* SVC cannot be taken from handler mode; already in interrupt
	 * context, so run the routine directly. Without the semaphore of the
	 * non-nested irq_offload(), CPUs may offload concurrently.
	 */
	if (k_is_in_isr()) {
		routine(parameter);
		return;
	}
#endif

	k_sched_lock();

	unsigned int cpu = OFFLOAD_CPU;

	offload_routine[cpu] = routine;
	offload_param[cpu] = parameter;

	__asm__ volatile ("svc %[id]\n"
			  IF_ENABLED(CONFIG_ARM_BTI, ("bti"))
			  :
			  : [id] "i" (_SVC_CALL_IRQ_OFFLOAD)
			  : "memory");

	offload_routine[cpu] = NULL;
	k_sched_unlock();
}

void arch_irq_offload_init(void)
{
}
