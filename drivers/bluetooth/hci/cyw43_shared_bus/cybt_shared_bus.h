/*
 * Infineon CYW43 "BT over shared bus" transport (port target).
 * SPDX-License-Identifier: Apache-2.0
 *
 * P0 skeleton: declarations only. The real register-level protocol
 * (HOST2BT / BT2HOST buffers + handshake) is ported from pico-sdk
 * cybt_shared_bus in a later phase (see docs pico-zephyr/bt-driver-design.md).
 */
#ifndef ZEPHYR_DRIVERS_BLUETOOTH_HCI_CYW43_CYBT_SHARED_BUS_H_
#define ZEPHYR_DRIVERS_BLUETOOTH_HCI_CYW43_CYBT_SHARED_BUS_H_

#include <stddef.h>
#include <stdint.h>

/* Bring the shared-bus BT transport up (load FW patch, init buffers). */
int cybt_shared_bus_init(void);

/* Write one HCI packet (type byte + payload) to the controller. */
int cybt_shared_bus_write(const uint8_t *data, size_t len);

/* Read one pending HCI packet from the controller, if any.
 * Returns number of bytes read, 0 if none, negative on error.
 */
int cybt_shared_bus_read(uint8_t *data, size_t max_len);

#endif /* ZEPHYR_DRIVERS_BLUETOOTH_HCI_CYW43_CYBT_SHARED_BUS_H_ */
