/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief SMP support for Cortex-M (ARMv8-M Mainline with USE_SWITCH)
 *
 * The SoC provides:
 *   - <soc_cpu_id.h> with soc_cpu_id(), the index of the running core,
 *     used by arch_curr_cpu();
 *   - z_arm_soc_cpu_start(), which releases a secondary core so that it
 *     starts executing @p entry with MSP = @p msp and VTOR = this core's;
 *   - the IPI: arch_sched_directed_ipi(), arch_sched_broadcast_ipi() and
 *     arch_smp_init().
 *
 * A secondary core repeats the per-core part of the primary's boot
 * (z_prep_c() / arch_kernel_init()): coprocessors and FPU, exception
 * priorities, fault and idle setup, NVIC priorities (the NVIC is banked
 * per core) and the arm-m-switch per-CPU state. It then moves thread mode
 * onto PSP, keeping MSP as its interrupt stack, before entering the kernel:
 * arm_m_switch() swaps whatever SP is active, so the first switch out of
 * the dummy thread must already run on PSP.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <cmsis_core.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/barrier.h>
#include <kernel_internal.h>
#include <kernel_arch_func.h>
#include <cortex_m/exception.h>

#define BOOT_STACK_SIZE 1024

/* Stack the secondary runs the kernel's start function on, until its
 * first context switch. Index 0 is unused.
 */
K_KERNEL_STACK_ARRAY_DEFINE(arm_m_boot_stacks, CONFIG_MP_MAX_NUM_CPUS, BOOT_STACK_SIZE);

static volatile struct {
	arch_cpustart_t fn;
	void *arg;
	int cpu;
} boot_params;

extern int z_arm_soc_cpu_start(int cpu_num, void (*entry)(void), uint32_t msp);
extern void arm_m_switch_cpu_init(int cpu);
#if defined(CONFIG_CPU_HAS_FPU)
extern void z_arm_floating_point_init(void);
#endif
#if defined(CONFIG_ARM_MPU)
extern int z_arm_mpu_init(void);
extern void z_arm_configure_static_mpu_regions(void);
#endif

static FUNC_NORETURN void secondary_entry(void)
{
	int cpu = boot_params.cpu;
	uint32_t psp = (uint32_t)K_KERNEL_STACK_BUFFER(arm_m_boot_stacks[cpu]) +
		       K_KERNEL_STACK_SIZEOF(arm_m_boot_stacks[cpu]);
	uint32_t msp = (uint32_t)K_KERNEL_STACK_BUFFER(z_interrupt_stacks[cpu]) +
		       K_KERNEL_STACK_SIZEOF(z_interrupt_stacks[cpu]);
	arch_cpustart_t fn;
	void *arg;

#ifdef CONFIG_SOC_RESET_HOOK
	/* What the primary ran at reset (e.g. enable coprocessors). */
	soc_reset_hook();
#endif
#if defined(CONFIG_CPU_HAS_FPU)
	z_arm_floating_point_init();
#endif
	z_arm_exc_setup();
	z_arm_fault_init();
	z_arm_cpu_idle_init();
	z_arm_clear_faults();
#if defined(CONFIG_ARM_CUSTOM_INTERRUPT_CONTROLLER)
	z_soc_irq_init();
#else
	z_arm_interrupt_init();
#endif
#if defined(CONFIG_ARM_MPU)
	z_arm_mpu_init();
	z_arm_configure_static_mpu_regions();
#endif
	arm_m_switch_cpu_init(cpu);
#ifdef CONFIG_THREAD_LOCAL_STORAGE
	/* The kernel sets the current thread (through TLS) before this CPU's
	 * first switch: give it the idle thread's TLS area meanwhile.
	 */
	_kernel.cpus[cpu].arch.tls_ptr = z_idle_threads[cpu].tls;
#endif
	soc_per_core_init_hook();

	fn = boot_params.fn;
	arg = boot_params.arg;
	barrier_dmem_fence_full();
	/* Release arch_cpu_start() on the primary. */
	boot_params.fn = NULL;

	/* The kernel enters secondaries with interrupts masked (it sets up
	 * the dummy thread through _current_cpu, which must not be mobile);
	 * the first switch to a real thread unmasks them. A Cortex-M comes
	 * out of reset with BASEPRI and PRIMASK clear.
	 */
	(void)arch_irq_lock();

	/* Thread mode on PSP. This function ran on MSP, so put MSP back at
	 * the top of the interrupt stack: arm_m_exc_tail() expects the topmost
	 * ISR's stacked EXC_RETURN exactly there (exc_lr_ptr), as the primary
	 * arranges when it switches to the main thread.
	 */
	__asm__ volatile("msr psp, %0\n"
			 "mrs r1, control\n"
			 "orr r1, r1, #2\n"
			 "msr control, r1\n"
			 "isb\n"
			 "msr msp, %1\n"
			 "mov r0, %3\n"
			 "blx %2\n"
			 :
			 : "r"(psp), "r"(msp), "r"(fn), "r"(arg)
			 : "r0", "r1", "memory");

	CODE_UNREACHABLE;
}

void arch_cpu_start(int cpu_num, k_thread_stack_t *stack, int sz, arch_cpustart_t fn, void *arg)
{
	/* Same top-of-stack formula as z_arm_interrupt_stack_setup() and
	 * arm_m_switch_cpu_init(), which locates the stacked EXC_RETURN there.
	 */
	uint32_t msp = (uint32_t)K_KERNEL_STACK_BUFFER(z_interrupt_stacks[cpu_num]) +
		       K_KERNEL_STACK_SIZEOF(z_interrupt_stacks[cpu_num]);

	ARG_UNUSED(stack);
	ARG_UNUSED(sz);

#ifdef CONFIG_INIT_STACKS
	/* reset.S only fills the primary's interrupt stack */
	(void)memset(K_KERNEL_STACK_BUFFER(z_interrupt_stacks[cpu_num]), 0xaa,
		     K_KERNEL_STACK_SIZEOF(z_interrupt_stacks[cpu_num]));
#endif

	boot_params.cpu = cpu_num;
	boot_params.arg = arg;
	barrier_dmem_fence_full();
	boot_params.fn = fn;
	barrier_dmem_fence_full();

	if (z_arm_soc_cpu_start(cpu_num, secondary_entry, msp) != 0) {
		return;
	}

	while (boot_params.fn != NULL) {
	}
}
