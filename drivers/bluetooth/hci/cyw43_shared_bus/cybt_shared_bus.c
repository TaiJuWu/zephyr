/*
 * Infineon CYW43 "BT over shared bus" transport (port target).
 * SPDX-License-Identifier: Apache-2.0
 *
 * P0 skeleton: stubbed. Real implementation (ported from pico-sdk
 * src/rp2_common/pico_cyw43_driver/cybt_shared_bus) lands in P2.
 */
#include "cybt_shared_bus.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(bt_cyw43, CONFIG_BT_HCI_DRIVER_LOG_LEVEL);

int cybt_shared_bus_init(void)
{
	LOG_WRN("cybt_shared_bus_init: stub (P0 skeleton)");
	return 0;
}

int cybt_shared_bus_write(const uint8_t *data, size_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOSYS;
}

int cybt_shared_bus_read(uint8_t *data, size_t max_len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(max_len);
	return 0;
}
