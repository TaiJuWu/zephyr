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

/*
 * ---------------------------------------------------------------------------
 * Radio bus-sharing policy (build-time) & next plan
 * ---------------------------------------------------------------------------
 * The CYW43439 shares ONE gSPI between Wi-Fi and BT. We avoid the WiFi+BT
 * simultaneous bus-arbitration problem ("P6") by letting only ONE radio be
 * active per build, selected via Kconfig choice CYW43_RADIO_USAGE:
 *   - CYW43_RADIO_WIFI : Wi-Fi only, this driver off.
 *   - CYW43_RADIO_BT   : this driver on; WIFI_AIROC stays compiled ONLY to
 *                        bring the CYW43 chip/bus up, no Wi-Fi traffic.
 * So the P6 pieces below are intentionally NOT implemented yet:
 *
 * TODO(P6, if WiFi+BT must run together): see bt-driver-design.md §7.1/§7.2
 *   1. shared bus lock covering the compound (set-backplane-window + transfer),
 *      taken by BOTH the WHD Wi-Fi path and this BT path (WHD has no bus lock;
 *      it assumes single-threaded access).
 *   2. backplane-window guard (window is chip-global shared state).
 *   3. port cybt CYBT_CORRUPTION_TEST detection/retry (chip-level corruption
 *      still occurs under full load even when serialized).
 *   4. radio/antenna coex: check Murata-1YN NVRAM btc_mode (else BLE TX ~-30dB).
 *
 * TODO(option-b, true Wi-Fi-free BT-only build): extract CYW43 chip/bus
 *   bring-up out of WHD so WIFI_AIROC can be =n. See bt-driver-design.md §9.6/§9.8.
 *
 * RX: adaptive poll (fast 1 ms after activity, back off to 50 ms idle) +
 *   TX kick (send() wakes the RX thread). A dedicated RX interrupt is not
 *   usable: GPIO24 is the shared SPI-data/host-wake pin managed by WHD, so
 *   a piggybacked host-wake callback almost never fires (kept opportunistic).
 *   See bt-driver-design.md section 8 / 3.4.
 * TODO(ext-adv): CYW43439 FW has no BT5 extended advertising; guard BT_EXT_ADV.
 * ---------------------------------------------------------------------------
 */

#define DT_DRV_COMPAT infineon_cyw43_bt_hci

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/drivers/gpio.h>

#include "port/cyw43_ll.h"
#include "port/cyw43_btbus.h"

/* WHD: borrow the handle from the AIROC Wi-Fi driver (must be up). */
#include <whd.h>
#include <whd_int.h>
extern whd_interface_t airoc_wifi_get_whd_interface(void);

/* Host-wake OOB line: the AIROC Wi-Fi driver owns this GPIO and configures
 * it as an interrupt. We piggyback a second GPIO callback so the controller
 * wakes the BT RX thread instead of it polling. No DT change needed.
 */
#define CYW43_AIROC_NODE DT_INST(0, infineon_airoc_wifi)
static const struct gpio_dt_spec cyw43_bt_host_wake =
	GPIO_DT_SPEC_GET_OR(CYW43_AIROC_NODE, wifi_host_wake_gpios, {0});

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bt_cyw43);

/* cyw43 shared-bus packet header: 4 bytes, HCI (H:4) type in byte[3]. */
#define CYW43_BT_HDR_SIZE 4

/* H:4 packet type indicators. */
#define H4_CMD 0x01
#define H4_ACL 0x02
#define H4_SCO 0x03
#define H4_EVT 0x04
#define H4_ISO 0x05

#define CYW43_BT_RX_STACK 2048
#define CYW43_BT_BUF_MAX  (CYW43_BT_HDR_SIZE + 255 + 4)

struct cyw43_bt_data {
	struct bt_hci_driver_data common; /* keep first (bt_hci_recv uses dev->data) */
	cyw43_ll_t ll;
	struct k_thread rx_thread;
	struct k_sem rx_sem;
	struct gpio_callback host_wake_cb;
	bool running;
	bool irq_rx;
};

static K_THREAD_STACK_DEFINE(cyw43_bt_rx_stack, CYW43_BT_RX_STACK);
static const struct device *cyw43_bt_dev;

/* Build a Zephyr RX net_buf from a payload (no H:4 type byte). */
static struct net_buf *cyw43_bt_alloc_rx(uint8_t h4_type, const uint8_t *payload,
					 uint16_t plen)
{
	struct net_buf *buf;

	switch (h4_type) {
	case H4_EVT:
		if (plen < 2) {
			return NULL;
		}
		buf = bt_buf_get_evt(payload[0], false, K_FOREVER);
		break;
	case H4_ACL:
		buf = bt_buf_get_rx(BT_BUF_ACL_IN, K_FOREVER);
		break;
	case H4_ISO:
		buf = bt_buf_get_rx(BT_BUF_ISO_IN, K_FOREVER);
		break;
	default:
		LOG_WRN("rx: unknown H:4 type 0x%02x", h4_type);
		return NULL;
	}

	if (buf == NULL) {
		return NULL;
	}
	if (net_buf_tailroom(buf) < plen) {
		LOG_WRN("rx: buf too small (%u < %u)", net_buf_tailroom(buf), plen);
		net_buf_unref(buf);
		return NULL;
	}
	net_buf_add_mem(buf, payload, plen);
	return buf;
}

static void cyw43_bt_host_wake_isr(const struct device *port,
				   struct gpio_callback *cb, uint32_t pins)
{
	struct cyw43_bt_data *d = CONTAINER_OF(cb, struct cyw43_bt_data, host_wake_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	k_sem_give(&d->rx_sem);
}

static void cyw43_bt_rx_fn(void *a, void *b, void *c)
{
	struct cyw43_bt_data *d = a;
	static uint8_t rxbuf[CYW43_BT_BUF_MAX] __aligned(4);
	/* No usable dedicated RX interrupt here: GPIO24 is the shared SPI-data /
	 * host-wake pin, armed only inside WHD's own idle windows, so a
	 * piggybacked callback almost never fires (measured: relying on it gave
	 * ~100 ms/command). Use adaptive polling with a TX kick: poll fast right
	 * after activity (command responses, RX bursts) and back off when idle.
	 * rx_sem is given by send() (TX kick) and, opportunistically, by the
	 * host-wake callback on the rare occasions it does fire.
	 */
	bool fast = true;
	uint32_t idle = 0;

	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (d->running) {
		int woken = k_sem_take(&d->rx_sem, fast ? K_MSEC(1) : K_MSEC(50));
		bool got = false;

		if (woken == 0) {
			fast = true;
			idle = 0;
		}

		/* Drain everything currently pending before sleeping again. */
		for (;;) {
			uint32_t len = 0;
			int err = cyw43_btbus_read(rxbuf, sizeof(rxbuf), &len);

			if (err == 0 && len > CYW43_BT_HDR_SIZE) {
				struct net_buf *nb = cyw43_bt_alloc_rx(
					rxbuf[3], &rxbuf[CYW43_BT_HDR_SIZE],
					(uint16_t)(len - CYW43_BT_HDR_SIZE));
				if (nb) {
					bt_hci_recv(cyw43_bt_dev, nb);
				}
				got = true;
			} else {
				break;
			}
		}

		if (got) {
			fast = true;
			idle = 0;
		} else if (fast && ++idle >= 20U) {
			fast = false;
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

	LOG_INF("CYW43 BT: download firmware / init shared bus");
	int err = cyw43_btbus_init(&d->ll);

	if (err) {
		LOG_ERR("cyw43_btbus_init failed: %d", err);
		return -EIO;
	}

	k_sem_init(&d->rx_sem, 0, 1);
	d->irq_rx = false;
	if (cyw43_bt_host_wake.port != NULL && gpio_is_ready_dt(&cyw43_bt_host_wake)) {
		/* Do NOT reconfigure the pin; AIROC already set it up as an IRQ. */
		gpio_init_callback(&d->host_wake_cb, cyw43_bt_host_wake_isr,
				   BIT(cyw43_bt_host_wake.pin));
		if (gpio_add_callback_dt(&cyw43_bt_host_wake, &d->host_wake_cb) == 0) {
			d->irq_rx = true;
			LOG_INF("CYW43 BT: host-wake callback on GPIO%u (opportunistic; "
				"RX is adaptive poll + TX kick)", cyw43_bt_host_wake.pin);
		} else {
			LOG_WRN("CYW43 BT: host-wake add_callback failed; polling");
		}
	} else {
		LOG_INF("CYW43 BT: no host-wake GPIO; RX polling");
	}

	cyw43_bt_dev = dev;
	d->running = true;
	k_thread_create(&d->rx_thread, cyw43_bt_rx_stack,
			K_THREAD_STACK_SIZEOF(cyw43_bt_rx_stack),
			cyw43_bt_rx_fn, d, NULL, NULL,
			K_PRIO_COOP(7), 0, K_NO_WAIT);
	k_thread_name_set(&d->rx_thread, "cyw43_bt_rx");
	LOG_INF("CYW43 BT: open OK");
	return 0;
}

static int cyw43_bt_send(const struct device *dev, struct net_buf *buf)
{
	static uint8_t txbuf[CYW43_BT_BUF_MAX] __aligned(4);
	struct cyw43_bt_data *d = dev->data;
	uint8_t h4_type;
	uint16_t plen;

	/* Modern Zephyr HCI TX buffers are H:4 encoded: data[0] is the type. */
	if (buf->len < 1) {
		net_buf_unref(buf);
		return -EINVAL;
	}
	h4_type = buf->data[0];
	plen = buf->len - 1;

	if ((uint32_t)plen + CYW43_BT_HDR_SIZE > sizeof(txbuf)) {
		net_buf_unref(buf);
		return -EMSGSIZE;
	}

	txbuf[0] = 0;
	txbuf[1] = 0;
	txbuf[2] = 0;
	txbuf[3] = h4_type;
	memcpy(&txbuf[CYW43_BT_HDR_SIZE], &buf->data[1], plen);

	int err = cyw43_btbus_write(txbuf, CYW43_BT_HDR_SIZE + plen);

	/* TX kick: wake the RX thread so the command response is read promptly
	 * without waiting for the idle poll (no reliable RX IRQ on this pin).
	 */
	k_sem_give(&d->rx_sem);

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
