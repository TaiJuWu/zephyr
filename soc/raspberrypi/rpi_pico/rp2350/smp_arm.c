/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief SMP support for the RP2350 Cortex-M33 cores
 *
 * - z_arm_soc_cpu_start(): start core 1 with the bootrom's SIO FIFO launch
 *   protocol (VTOR, SP, entry with the Thumb bit).
 * - IPIs over the SIO doorbells (SIO_IRQ_BELL), as on the Hazard3 cores.
 * - soc_per_core_init_hook(): enable the doorbell IRQ on core 1 (the NVIC is
 *   banked per core; device IRQs stay on core 0).
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/platform/hooks.h>
#include <cmsis_core.h>
#include <ipi.h>

#include <hardware/structs/sio.h>
#include <hardware/regs/intctrl.h>
#include <hardware/sync.h>

#define IPI_DOORBELL BIT(0)

/* CPUs that take doorbell IPIs. Until core 1 runs Zephyr it must not be
 * rung: it is parked in the bootrom, and a doorbell left pending fires as
 * soon as it enables SIO_IRQ_BELL, before its kernel state exists.
 */
static atomic_t ipi_online = ATOMIC_INIT(BIT(0));

static void fifo_drain(void)
{
	while (sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS) {
		(void)sio_hw->fifo_rd;
	}
}

static void fifo_push(uint32_t value)
{
	while (!(sio_hw->fifo_st & SIO_FIFO_ST_RDY_BITS)) {
	}
	sio_hw->fifo_wr = value;
	/* Core 1 waits in the bootrom with WFE. */
	__sev();
}

static uint32_t fifo_pop(void)
{
	while (!(sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS)) {
	}
	return sio_hw->fifo_rd;
}

int z_arm_soc_cpu_start(int cpu_num, void (*entry)(void), uint32_t msp)
{
	/* Bootrom launch sequence: core 1 echoes each word, any mismatch
	 * restarts it. Core 1 shares the vector table with core 0.
	 */
	const uint32_t seq[] = {
		0, 0, 1, SCB->VTOR, msp, (uintptr_t)entry | 1,
	};
	unsigned int i = 0;

	if (cpu_num != 1) {
		return -EINVAL;
	}

	while (i < ARRAY_SIZE(seq)) {
		if (seq[i] == 0) {
			fifo_drain();
		}
		fifo_push(seq[i]);
		i = (fifo_pop() == seq[i]) ? (i + 1) : 0;
	}

	return 0;
}

void arch_sched_directed_ipi(uint32_t cpu_bitmap)
{
	unsigned int key = arch_irq_lock();
	unsigned int id = _current_cpu->id;

	cpu_bitmap &= (uint32_t)atomic_get(&ipi_online);

	for (unsigned int i = 0; i < arch_num_cpus(); i++) {
		if ((i != id) && ((cpu_bitmap & BIT(i)) != 0)) {
			/* With two cores, an outgoing doorbell rings the other one. */
			sio_hw->doorbell_out_set = IPI_DOORBELL;
		}
	}

	arch_irq_unlock(key);
}

void arch_sched_broadcast_ipi(void)
{
	arch_sched_directed_ipi(IPI_ALL_CPUS_MASK);
}

static void doorbell_isr(const void *arg)
{
	ARG_UNUSED(arg);

	sio_hw->doorbell_in_clr = IPI_DOORBELL;
	z_sched_ipi();
}

int arch_smp_init(void)
{
	IRQ_CONNECT(SIO_IRQ_BELL, 0, doorbell_isr, NULL, 0);
	irq_enable(SIO_IRQ_BELL);

	return 0;
}

void soc_per_core_init_hook(void)
{
	/* Core 0 enables the doorbell in arch_smp_init(). */
	if (arch_proc_id() != 0) {
		sio_hw->doorbell_in_clr = IPI_DOORBELL;
		irq_enable(SIO_IRQ_BELL);
		atomic_or(&ipi_online, BIT(arch_proc_id()));
	}
}
