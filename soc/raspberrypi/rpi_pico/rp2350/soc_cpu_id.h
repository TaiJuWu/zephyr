/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_RASPBERRYPI_RP2350_SOC_CPU_ID_H_
#define ZEPHYR_SOC_RASPBERRYPI_RP2350_SOC_CPU_ID_H_

#include <stdint.h>
#include <zephyr/toolchain.h>

/* SIO CPUID: a core-local register that reads 0 on core 0 and 1 on core 1. */
#define RP2350_SIO_CPUID ((volatile uint32_t *)0xd0000000UL)

static ALWAYS_INLINE uint32_t soc_cpu_id(void)
{
	return *RP2350_SIO_CPUID;
}

#endif
