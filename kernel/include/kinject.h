/*
 * Copyright (c) 2026 TaiJu Wu
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_KERNEL_INCLUDE_KINJECT_H_
#define ZEPHYR_KERNEL_INCLUDE_KINJECT_H_

/* Z_INJECT(point, arg) marks a test injection point, see
 * <zephyr/ztest_inject.h> for the list. It compiles to nothing unless
 * CONFIG_ZTEST_INJECT is set.
 */
#ifdef CONFIG_ZTEST_INJECT
#include <zephyr/ztest_inject.h>
#define Z_INJECT(point, arg) z_ztest_inject(ZTEST_INJECT_##point, (void *)(arg))
#else
#define Z_INJECT(point, arg) do { } while (false)
#endif

#endif /* ZEPHYR_KERNEL_INCLUDE_KINJECT_H_ */
