/* Zephyr compat config for the ported cybt_shared_bus files. */
#ifndef CYW43_COMPAT_CONFIG_H_
#define CYW43_COMPAT_CONFIG_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/__assert.h>

#define CYW43_USE_HEX_BTFW        0
#define CYW43_RESOURCE_ATTRIBUTE
#define CYW43_BUS_MAX_BLOCK_SIZE  64

#define CYW43_THREAD_ENTER
#define CYW43_THREAD_EXIT

#define CYW43_PRINTF(...)  printk(__VA_ARGS__)
#define CYW43_WARN(...)    printk("[cyw43-bt] " __VA_ARGS__)

#ifndef assert
#define assert(x)          __ASSERT_NO_MSG(x)
#endif
#ifndef panic
#define panic(...)         do { printk("[cyw43-bt] panic: " __VA_ARGS__); k_oops(); } while (0)
#endif

#endif
