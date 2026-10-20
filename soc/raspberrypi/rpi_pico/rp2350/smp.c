/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief SMP support for the RP2350 Hazard3 cores
 *
 * - pm_cpu_on(): start core 1 with the bootrom's SIO FIFO launch protocol.
 * - IPIs are sent over the SIO doorbells (SIO_IRQ_BELL). The standard
 *   machine software interrupt is not usable here: the Hazard3 interrupt
 *   controller only routes external IRQs, so irq_enable(RISCV_IRQ_MSOFT)
 *   would enable external IRQ 3 (TIMER0_IRQ_3) instead.
 * - soc_per_core_init_hook(): enable external interrupts and the doorbell
 *   IRQ on core 1 (the Hazard3 enable CSRs are per core).
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/pm_cpu_ops.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/arch/riscv/csr.h>
#include <ipi.h>

#include <hardware/structs/sio.h>
#include <hardware/regs/intctrl.h>
#include <hardware/regs/rvcsr.h>
#include <hardware/sync.h>

#define IPI_DOORBELL BIT(0)

/* Set by arch_cpu_start() before calling pm_cpu_on(). */
extern volatile void *riscv_cpu_sp;
extern void _isr_wrapper(void);

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
	/* Core 1 waits in the bootrom with h3.block. */
	__sev();
}

static uint32_t fifo_pop(void)
{
	while (!(sio_hw->fifo_st & SIO_FIFO_ST_VLD_BITS)) {
	}
	return sio_hw->fifo_rd;
}

int pm_cpu_on(unsigned long cpuid, uintptr_t entry_point)
{
	/*
	 * Bootrom launch sequence: core 1 echoes each word, and any mismatch
	 * restarts it. The stack is the one arch_cpu_start() is about to hand
	 * over in riscv_cpu_sp; __start runs soc_reset_hook() on it before
	 * reset.S parks the hart in boot_secondary_core.
	 */
	const uint32_t seq[] = {
		0, 0, 1, (uintptr_t)_isr_wrapper, (uintptr_t)riscv_cpu_sp, entry_point,
	};
	unsigned int i = 0;

	if (cpuid != 1) {
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

	for (unsigned int i = 0; i < arch_num_cpus(); i++) {
		if ((i != id) && _kernel.cpus[i].arch.online && ((cpu_bitmap & BIT(i)) != 0)) {
			/* With two cores, an outgoing doorbell rings the other one. */
			sio_hw->doorbell_out_set = IPI_DOORBELL;
		}
	}

	arch_irq_unlock(key);
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
	/* Core 0 is set up by the interrupt controller driver and arch_smp_init(). */
	if (arch_proc_id() == 0) {
		return;
	}

	csr_set(mie, RVCSR_MIE_MEIE_BITS);
	irq_enable(SIO_IRQ_BELL);
}
