/* Zephyr compat shim exposing the small cyw43_ll surface that the ported
 * cybt_shared_bus files use, backed by the AIROC WHD bus on Zephyr.
 */
#ifndef CYW43_COMPAT_LL_H_
#define CYW43_COMPAT_LL_H_

#include <stdint.h>
#include <stddef.h>
#include "cyw43_config.h"

struct whd_driver;
typedef struct _cyw43_ll_t {
	struct whd_driver *whd; /* whd_driver_t */
} cyw43_ll_t;

/* Backplane register/memory access (implemented over WHD). */
void     cyw43_ll_write_backplane_reg(cyw43_ll_t *self, uint32_t addr, uint32_t val);
uint32_t cyw43_ll_read_backplane_reg(cyw43_ll_t *self, uint32_t addr);
void     cyw43_ll_write_backplane_mem(cyw43_ll_t *self, uint32_t addr, uint32_t len, const uint8_t *buf);
void     cyw43_ll_read_backplane_mem(cyw43_ll_t *self, uint32_t addr, uint32_t len, uint8_t *buf);

/* Misc helpers used by the ported code. */
void cyw43_delay_ms(uint32_t ms);
void *cyw43_malloc(size_t size);
void cyw43_free(void *ptr);

#endif
