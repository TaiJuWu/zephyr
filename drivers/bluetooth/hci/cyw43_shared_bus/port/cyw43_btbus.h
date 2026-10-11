/* Declarations for the ported cyw43 shared-bus BT HCI framing layer
 * (defined in cybt_shared_bus.c). SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef CYW43_COMPAT_BTBUS_H_
#define CYW43_COMPAT_BTBUS_H_
#include "cyw43_ll.h"
int cyw43_btbus_init(cyw43_ll_t *self);
int cyw43_btbus_write(uint8_t *buf, size_t len);   /* buf has 4-byte header; type in buf[3] */
int cyw43_btbus_read(uint8_t *buf, uint32_t max_size, uint32_t *len);
#endif
