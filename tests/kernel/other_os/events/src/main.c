/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Event tests of other RTOSes, ported to k_event (see README.rst):
 * - RT-Thread src/utest/event_tc.c
 * - NuttX apps/testing/ostest/nxevent.c
 * - RIOT tests/core/thread_flags, tests/core/thread_flags_group
 *
 * The originals run on one CPU; on SMP the other CPUs are held during each
 * test, as in the ThreadX port.
 */

#include <zephyr/ztest.h>

ZTEST_SUITE(other_os_events, NULL, NULL, ztest_simple_1cpu_before,
	    ztest_simple_1cpu_after, NULL);
