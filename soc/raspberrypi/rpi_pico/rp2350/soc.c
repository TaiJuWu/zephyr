/*
 * Copyright (c) 2024 Andrew Featherstone
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief System/hardware module for Raspberry Pi RP235xx MCUs
 *
 * This module provides routines to initialize and support board-level hardware
 * for the Raspberry Pi RP235xx (RP2350A, RP2350B, RP2354A, RP2354B).
 */

#if CONFIG_SOC_RESET_HOOK
#include <pico/runtime_init.h>
#if CONFIG_RISCV
#include <hardware/riscv_platform_timer.h>
#endif

#if defined(CONFIG_ARM) && defined(CONFIG_SMP) && !defined(CONFIG_ARM_MPU)
#include <cmsis_core.h>
#include <hardware/regs/addressmap.h>

/* The M33 only sends exclusive accesses to the bus, where the RP2350 global
 * exclusive monitor sees them, for Shareable memory; in the default memory
 * map SRAM is Non-shareable, so LDREX/STREX (atomics, spinlocks) would only
 * be exclusive per core. Map SRAM as Normal, Shareable. The MPU is banked,
 * so each core runs this (core 1 from arch/arm/core/cortex_m/smp.c).
 */
static void rp2350_share_sram(void)
{
	MPU->CTRL = 0;
	MPU->MAIR0 = 0xff;	/* Attr0: Normal, write-back, RW-allocate */
	MPU->RNR = 0;
	MPU->RBAR = SRAM_BASE | (3U << MPU_RBAR_SH_Pos) | (1U << MPU_RBAR_AP_Pos);
	MPU->RLAR = ((SRAM_END - 1U) & MPU_RLAR_LIMIT_Msk) | MPU_RLAR_EN_Msk;
	MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
	__DSB();
	__ISB();
}
#endif

void soc_reset_hook(void)
{
#if CONFIG_RISCV
	/* The kernel requires the mtimer to tick at the CPU frequency. */
	riscv_timer_set_fullspeed(true);
#else
	runtime_init_per_core_enable_coprocessors();
#if defined(CONFIG_SMP) && !defined(CONFIG_ARM_MPU)
	rp2350_share_sram();
#endif
#endif
}

#endif /* CONFIG_SOC_RESET_HOOK */
