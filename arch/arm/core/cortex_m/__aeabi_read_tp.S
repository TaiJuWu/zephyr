/*
 * Copyright (c) 2020 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>

_ASM_FILE_PROLOGUE

GTEXT(__aeabi_read_tp)

GDATA(z_arm_tls_ptr)

/* Grab the TLS pointer and store in R0.
 * According to the Run-Time ABI for the Arm® Architecture section 5.3.5, this
 * function may only clobber r0, ip, lr & CPSR.
 *
 * This can only be guaranteed by either implementing a naked C function with
 * inline assembly, or plain assembly.
 */
SECTION_FUNC(TEXT, __aeabi_read_tp)
#ifdef CONFIG_SMP
	/* Per-CPU pointer. The run-time ABI lets this clobber only r0, so keep
	 * the caller-saved registers and the flags around the C helper.
	 */
	push {r1-r4, r12, lr}
	mrs r4, apsr
	bl z_arm_smp_tls_ptr_get
	msr apsr_nzcvq, r4
	pop {r1-r4, r12, pc}
#else
	ldr r0, =z_arm_tls_ptr
	ldr r0, [r0]
	bx lr
#endif
