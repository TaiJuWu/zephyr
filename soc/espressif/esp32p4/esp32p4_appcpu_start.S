/*
 * Copyright (c) 2026 TaiJu Wu <tjwu1217@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * App CPU entry, released by the ROM through the app CPU boot address. It
 * does the part of the generic RISC-V secondary start in reset.S that the
 * ESP32-P4 boot path skips: wait for the wake flag, take the stack from
 * arch_cpu_start() and hand over to arch_secondary_cpu_init().
 */

#include <zephyr/toolchain.h>
#include <zephyr/arch/cpu.h>

GTEXT(esp32p4_appcpu_entry)
GTEXT(arch_secondary_cpu_init)
GDATA(riscv_cpu_wake_flag)
GDATA(riscv_cpu_boot_flag)
GDATA(riscv_cpu_sp)

	.section .iram1, "ax"
	.balign 4

esp32p4_appcpu_entry:
#ifdef CONFIG_RISCV_GP
	.option push
	.option norelax
	la gp, __global_pointer$
	.option pop
#endif
	csrci mstatus, MSTATUS_MIE

#ifdef CONFIG_FPU
	li t0, MSTATUS_FS_INIT
	csrs mstatus, t0
	fscsr zero
#endif

	csrr a0, mhartid

	la t0, riscv_cpu_wake_flag
	li t1, -1
	sw t1, 0(t0)
	la t0, riscv_cpu_boot_flag
	sw zero, 0(t0)

1:
	la t0, riscv_cpu_wake_flag
	lw t0, 0(t0)
	bne a0, t0, 1b

	la t0, riscv_cpu_sp
	lw sp, 0(t0)

#ifdef CONFIG_THREAD_LOCAL_STORAGE
	/* Zeroed TLS area until arch_secondary_cpu_init() points tp at the idle thread's. */
	mv s2, a0
	call z_tls_data_size_asm
	sub sp, sp, a0
	mv tp, sp
	mv t0, sp
	add t1, sp, a0
2:
	bgeu t0, t1, 3f
	sw zero, 0(t0)
	addi t0, t0, 4
	j 2b
3:
	mv a0, s2
#endif

	la t0, riscv_cpu_boot_flag
	li t1, 1
	sw t1, 0(t0)

	j arch_secondary_cpu_init
