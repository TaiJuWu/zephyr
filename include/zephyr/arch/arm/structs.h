/*
 * Copyright (c) 2023 Arm Limited (or its affiliates). All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_ARCH_ARM_STRUCTS_H_
#define ZEPHYR_INCLUDE_ARCH_ARM_STRUCTS_H_

#include <zephyr/types.h>

#if defined(CONFIG_CPU_AARCH32_CORTEX_A) || defined(CONFIG_CPU_AARCH32_CORTEX_R)
/* Per CPU architecture specifics */
struct _cpu_arch {
	int8_t exc_depth;
#if defined(CONFIG_ARM_TRACK_ACTIVE_IRQ)
	/*
	 * INTID currently being serviced on this CPU, biased by one so
	 * that the zero-initialized boot state reads as "none". Maintained
	 * by the ISR wrapper; read through arch_irq_get_active().
	 */
	uint32_t active_irq;
#endif
};

#elif defined(CONFIG_CPU_CORTEX_M) && defined(CONFIG_SMP) && defined(CONFIG_USE_SWITCH)

/* Pointers used by the Cortex-M interrupt-exit context switch
 * (arch/arm/core/cortex_m/arm-m-switch.c); see struct arm_m_cs_ptrs there.
 */
struct arm_m_cpu_cs_ptrs {
	void *out, *in, *lr_save, *lr_fixup;
};

/* Per CPU architecture specifics */
struct _cpu_arch {
	struct arm_m_cpu_cs_ptrs cs_ptrs;
	/* Stacked EXC_RETURN at the top of this CPU's interrupt stack */
	uint32_t *exc_lr_ptr;
#ifdef CONFIG_THREAD_LOCAL_STORAGE
	/* TLS pointer of the thread running here (z_arm_tls_ptr on UP) */
	uintptr_t tls_ptr;
#endif
};

#else

/* Default definitions when no architecture specific definitions exist. */

/* Per CPU architecture specifics (empty) */
struct _cpu_arch {
#ifdef __cplusplus
	/* An empty struct is not valid C, and compilers that accept it give
	 * it size 0 while C++ gives 1. Keep a byte so both languages agree.
	 */
	uint8_t dummy;
#endif
};

#endif

#endif /* ZEPHYR_INCLUDE_ARCH_ARM_STRUCTS_H_ */
