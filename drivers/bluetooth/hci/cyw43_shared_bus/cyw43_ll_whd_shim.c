/*
 * cyw43_ll -> AIROC WHD shim.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implements the tiny cyw43_ll backplane surface required by the ported
 * cybt_shared_bus code, on top of the WHD bus used by the Zephyr airoc
 * Wi-Fi driver. The WHD handle is borrowed from the airoc Wi-Fi driver,
 * which must be up (it powers the CYW43 and initialises the shared bus).
 */
#include <zephyr/kernel.h>
#include "port/cyw43_ll.h"

/* WHD */
#include <whd.h>
#include <bus_protocols/whd_bus_protocol_interface.h>

void cyw43_ll_write_backplane_reg(cyw43_ll_t *self, uint32_t addr, uint32_t val)
{
	whd_bus_write_backplane_value((whd_driver_t)self->whd, addr, 4, val);
}

uint32_t cyw43_ll_read_backplane_reg(cyw43_ll_t *self, uint32_t addr)
{
	uint32_t val = 0;

	whd_bus_read_backplane_value((whd_driver_t)self->whd, addr, 4, (uint8_t *)&val);
	return val;
}

void cyw43_ll_write_backplane_mem(cyw43_ll_t *self, uint32_t addr, uint32_t len,
				  const uint8_t *buf)
{
	whd_bus_mem_bytes((whd_driver_t)self->whd, BUS_WRITE, addr, (uint16_t)len,
			  (uint8_t *)buf);
}

void cyw43_ll_read_backplane_mem(cyw43_ll_t *self, uint32_t addr, uint32_t len,
				 uint8_t *buf)
{
	whd_bus_mem_bytes((whd_driver_t)self->whd, BUS_READ, addr, (uint16_t)len, buf);
}

void cyw43_delay_ms(uint32_t ms) { k_msleep((int32_t)ms); }
void *cyw43_malloc(size_t size)  { return k_malloc(size); }
void cyw43_free(void *ptr)       { k_free(ptr); }
