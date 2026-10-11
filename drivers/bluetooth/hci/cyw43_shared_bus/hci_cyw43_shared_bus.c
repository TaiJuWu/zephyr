/*
 * HCI driver for the Infineon CYW43439 Bluetooth controller, transported
 * over the CYW43 shared (gSPI) bus. Used on e.g. Raspberry Pi Pico 2 W.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * P0 skeleton: registers a bt_hci device and wires open/send to the
 * (stubbed) shared-bus transport. RX path and real transport come later
 * (see docs pico-zephyr/bt-driver-design.md).
 */

#define DT_DRV_COMPAT infineon_cyw43_bt_hci

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>

#include "cybt_shared_bus.h"

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bt_cyw43);

static int cyw43_bt_open(const struct device *dev)
{
	ARG_UNUSED(dev);

	LOG_INF("CYW43 shared-bus HCI: open (skeleton)");
	return cybt_shared_bus_init();
}

static int cyw43_bt_send(const struct device *dev, struct net_buf *buf)
{
	ARG_UNUSED(dev);

	LOG_WRN("CYW43 shared-bus HCI: send not implemented yet (type %u, len %u)",
		buf->data[0], buf->len);
	net_buf_unref(buf);
	return -ENOTSUP;
}

static DEVICE_API(bt_hci, cyw43_bt_api) = {
	.open = cyw43_bt_open,
	.send = cyw43_bt_send,
};

static int cyw43_bt_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	LOG_DBG("CYW43 shared-bus HCI: device init (skeleton)");
	return 0;
}

#define CYW43_BT_DEVICE_INIT(inst)                                             \
	static struct bt_hci_driver_data cyw43_bt_data_##inst;                 \
	static const struct bt_hci_driver_config cyw43_bt_config_##inst =      \
		BT_DT_HCI_DRIVER_CONFIG_INST_GET(inst);                       \
	DEVICE_DT_INST_DEFINE(inst, cyw43_bt_init, NULL,                       \
			      &cyw43_bt_data_##inst, &cyw43_bt_config_##inst, \
			      POST_KERNEL, CONFIG_BT_HCI_INIT_PRIORITY,       \
			      &cyw43_bt_api)

DT_INST_FOREACH_STATUS_OKAY(CYW43_BT_DEVICE_INIT)
