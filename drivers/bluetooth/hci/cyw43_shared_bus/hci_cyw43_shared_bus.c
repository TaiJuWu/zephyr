/*
 * HCI driver for the Infineon CYW43439 Bluetooth controller over the CYW43
 * shared (gSPI) bus. Used on e.g. Raspberry Pi Pico 2 W.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The shared-bus transport (cybt_shared_bus*, cyw43_btbus_*) is ported from
 * pico-sdk / cyw43-driver (BSD-3) under port/. The CYW43 chip + bus are
 * brought up by the AIROC Wi-Fi driver; we borrow its WHD handle.
 */

#define DT_DRV_COMPAT infineon_cyw43_bt_hci

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/drivers/bluetooth.h>

#include "port/cyw43_ll.h"
#include "port/cyw43_btbus.h"

/* Borrow the WHD handle from the AIROC Wi-Fi driver (must be up). */
#include <whd.h>
extern whd_interface_t airoc_wifi_get_whd_interface(void);

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bt_cyw43);

#define CYW43_BT_HDR_SIZE 4
#define CYW43_BT_RX_STACK 2048
#define CYW43_BT_RX_MAX   (CYW43_BT_HDR_SIZE + 255 + 4)

struct cyw43_bt_data {
	struct bt_hci_driver_data common; /* must be first (bt_hci_recv uses dev->data) */
	cyw43_ll_t ll;
	struct k_thread rx_thread;
	bool running;
};

static K_THREAD_STACK_DEFINE(cyw43_bt_rx_stack, CYW43_BT_RX_STACK);
static const struct device *cyw43_bt_dev;

static struct net_buf *cyw43_bt_make_rx(uint8_t type, const uint8_t *data, uint16_t len)
{
	switch (type) {
	case BT_HCI_H4_EVT:
		return bt_buf_get_evt(data[0], false, K_NO_WAIT) ?
			       ({ struct net_buf *b = bt_buf_get_evt(data[0], false, K_FOREVER);
				  net_buf_add_mem(b, data, len); b; }) : NULL;
	case BT_HCI_H4_ACL: {
		struct net_buf *b = bt_buf_get_rx(BT_BUF_ACL_IN, K_FOREVER);
		net_buf_add_mem(b, data, len);
		return b;
	}
	default:
		LOG_WRN("unknown HCI packet type 0x%02x", type);
		return NULL;
	}
}

static void cyw43_bt_rx_fn(void *a, void *b, void *c)
{
	struct cyw43_bt_data *d = a;
	static uint8_t rxbuf[CYW43_BT_RX_MAX] __aligned(4);

	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (d->running) {
		uint32_t len = 0;
		int err = cyw43_btbus_read(rxbuf, sizeof(rxbuf), &len);

		if (err == 0 && len > CYW43_BT_HDR_SIZE) {
			uint8_t type = rxbuf[3];
			struct net_buf *nb =
				cyw43_bt_make_rx(type, &rxbuf[CYW43_BT_HDR_SIZE],
						 len - CYW43_BT_HDR_SIZE);
			if (nb) {
				bt_hci_recv(cyw43_bt_dev, nb);
			}
		} else {
			k_msleep(2);
		}
	}
}

static int cyw43_bt_open(const struct device *dev)
{
	struct cyw43_bt_data *d = dev->data;
	whd_interface_t itf = airoc_wifi_get_whd_interface();

	if (itf == NULL) {
		LOG_ERR("AIROC Wi-Fi not up; enable Wi-Fi so the CYW43 bus is ready");
		return -ENODEV;
	}
	d->ll.whd = (struct whd_driver *)itf->whd_driver;

	LOG_INF("CYW43 BT: downloading firmware / init shared bus");
	int err = cyw43_btbus_init(&d->ll);
	if (err) {
		LOG_ERR("cyw43_btbus_init failed: %d", err);
		return -EIO;
	}

	cyw43_bt_dev = dev;
	d->running = true;
	k_thread_create(&d->rx_thread, cyw43_bt_rx_stack,
			K_THREAD_STACK_SIZEOF(cyw43_bt_rx_stack),
			cyw43_bt_rx_fn, d, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
	LOG_INF("CYW43 BT: open OK");
	return 0;
}

static int cyw43_bt_send(const struct device *dev, struct net_buf *buf)
{
	ARG_UNUSED(dev);
	static uint8_t txbuf[CYW43_BT_RX_MAX] __aligned(4);
	uint8_t type;

	switch (bt_buf_get_type(buf)) {
	case BT_BUF_CMD: type = BT_HCI_H4_CMD; break;
	case BT_BUF_ACL_OUT: type = BT_HCI_H4_ACL; break;
	default:
		LOG_WRN("unsupported tx buf type %d", bt_buf_get_type(buf));
		net_buf_unref(buf);
		return -ENOTSUP;
	}

	if (buf->len + CYW43_BT_HDR_SIZE > sizeof(txbuf)) {
		net_buf_unref(buf);
		return -EMSGSIZE;
	}
	txbuf[3] = type;
	memcpy(&txbuf[CYW43_BT_HDR_SIZE], buf->data, buf->len);
	int err = cyw43_btbus_write(txbuf, buf->len + CYW43_BT_HDR_SIZE);

	net_buf_unref(buf);
	return err ? -EIO : 0;
}

static DEVICE_API(bt_hci, cyw43_bt_api) = {
	.open = cyw43_bt_open,
	.send = cyw43_bt_send,
};

static int cyw43_bt_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define CYW43_BT_DEVICE_INIT(inst)                                             \
	static struct cyw43_bt_data cyw43_bt_data_##inst;                      \
	static const struct bt_hci_driver_config cyw43_bt_config_##inst =      \
		BT_DT_HCI_DRIVER_CONFIG_INST_GET(inst);                       \
	DEVICE_DT_INST_DEFINE(inst, cyw43_bt_init, NULL,                       \
			      &cyw43_bt_data_##inst, &cyw43_bt_config_##inst, \
			      POST_KERNEL, CONFIG_BT_HCI_INIT_PRIORITY,       \
			      &cyw43_bt_api)

DT_INST_FOREACH_STATUS_OKAY(CYW43_BT_DEVICE_INIT)
