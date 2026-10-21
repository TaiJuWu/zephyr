/*
 * Copyright (c) 2024 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/interrupt_controller/intc_esp32.h>

#include <soc.h>
#include <esp_log.h>
#include <esp_cpu.h>
#include <esp_rom_serial_output.h>

#include <esp_mcuboot_image.h>
#include <esp_memory_utils.h>
#include <hw_init.h>

#define TAG "amp"

#ifdef CONFIG_SMP

#include <ipi.h>
#include <soc/extmem_reg.h>
#include <soc/system_reg.h>
#include <zephyr/zsr.h>

struct cpustart_rec {
	arch_cpustart_t fn;
	void *arg;
	int vecbase;
	volatile int *alive;
};

static volatile struct cpustart_rec *start_rec;
__used static void *appcpu_top;
static bool cpus_active[CONFIG_MP_MAX_NUM_CPUS];

/* CPU n takes the cross-core interrupt FROM_CPU_INTRn: writing 1 to
 * SYSTEM_CPU_INTR_FROM_CPU_n_REG interrupts it, writing 0 acknowledges.
 */
static mem_addr_t ipi_reg(int cpu)
{
	return cpu == 0 ? SYSTEM_CPU_INTR_FROM_CPU_0_REG : SYSTEM_CPU_INTR_FROM_CPU_1_REG;
}

IRAM_ATTR static void esp_crosscore_isr(void *arg)
{
	ARG_UNUSED(arg);

	REG_WRITE(ipi_reg(esp_core_id()), 0);
	z_sched_ipi();
}

/* CPU interrupt number of the cross-core interrupt, the same on both CPUs.
 * The software ISR table is indexed by CPU interrupt number only, not per
 * CPU, so the APP CPU must not take a number that the PRO CPU uses for a
 * different source: it reuses the PRO CPU's IPI number, whose table entry
 * is this same ISR.
 */
static int ipi_intno = -1;

static void ipi_init_pro_cpu(void)
{
	intr_handle_t handle;

	REG_WRITE(ipi_reg(0), 0);
	esp_intr_alloc(DT_IRQ_BY_IDX(DT_NODELABEL(ipi0), 0, irq),
		       ESP_PRIO_TO_FLAGS(DT_IRQ_BY_IDX(DT_NODELABEL(ipi0), 0, priority)) |
			       ESP_INT_FLAGS_CHECK(DT_IRQ_BY_IDX(DT_NODELABEL(ipi0), 0, flags)) |
			       ESP_INTR_FLAG_IRAM,
		       esp_crosscore_isr, NULL, &handle);
	ipi_intno = esp_intr_get_intno(handle);
	esp_intr_reserve(ipi_intno, 1);
}

/* Runs on the APP CPU with interrupts masked. */
static void ipi_init_app_cpu(void)
{
	REG_WRITE(ipi_reg(1), 0);
	esp_rom_route_intr_matrix(1, DT_IRQ_BY_IDX(DT_NODELABEL(ipi1), 0, irq), ipi_intno);
	esp_cpu_intr_enable(BIT(ipi_intno));
}

__used static void appcpu_entry2(void)
{
	volatile int ie;

	/* Nothing enabled, VECBASE from the main CPU, and interrupts masked
	 * until the scheduler switches to this CPU's first thread.
	 */
	ie = 0;
	__asm__ volatile("wsr.INTENABLE %0" : : "r"(ie));
	__asm__ volatile("wsr.VECBASE %0" : : "r"(start_rec->vecbase));
	__asm__ volatile("rsync");

	_cpu_t *cpu = &_kernel.cpus[1];

	__asm__ volatile("wsr %0, " ZSR_CPU_STR : : "r"(cpu));

	ipi_init_app_cpu();

	*start_rec->alive = 1;
	start_rec->fn(start_rec->arg);
}

/* Entry point of the APP CPU, called by the ROM. Mask interrupts, start
 * from a clean register window (WINDOWBASE 0, only window 0 live), take the
 * interrupt stack Zephyr gave this CPU and call into C. A fresh window
 * state matters: a callee that spills its caller's registers must find a
 * valid frame there, not whatever the ROM left.
 */
void z_esp32s3_appcpu_asm_entry(void);
__asm__(".section .iram1.z_esp32s3_appcpu_asm_entry, \"ax\"\n\t"
	".literal_position\n\t"
	".align 4\n\t"
	".global z_esp32s3_appcpu_asm_entry\n\t"
	"z_esp32s3_appcpu_asm_entry:\n\t"
	"  movi  a0, 0x4002f\n\t" /* WOE | UM | INTLEVEL(max) */
	"  wsr   a0, PS\n\t"
	"  movi  a0, 0\n\t"
	"  wsr   a0, WINDOWBASE\n\t"
	"  movi  a0, 1\n\t"
	"  wsr   a0, WINDOWSTART\n\t"
	"  rsync\n\t"
	"  movi  a1, appcpu_top\n\t"
	"  l32i  a1, a1, 0\n\t"
	"  movi  a0, 0\n\t"
	"  call4 appcpu_entry2\n\t"
	"1: waiti 15\n\t"
	"  j     1b\n\t"
	".text\n\t");

static void smp_start_core1(void *entry_point)
{
	/* The L1 caches are shared, but each CPU has its own bus into them
	 * and the APP CPU's is shut at boot.
	 */
	REG_CLR_BIT(EXTMEM_ICACHE_CTRL1_REG, EXTMEM_ICACHE_SHUT_CORE1_BUS);
	REG_CLR_BIT(EXTMEM_DCACHE_CTRL1_REG, EXTMEM_DCACHE_SHUT_CORE1_BUS);

	esp_rom_ets_set_appcpu_boot_addr((void *)entry_point);

	REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
	REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
	REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETTING);
	REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETTING);
}

void arch_cpu_start(int cpu_num, k_thread_stack_t *stack, int sz, arch_cpustart_t fn, void *arg)
{
	volatile struct cpustart_rec sr;
	volatile int alive_flag = 0;
	int vb;

	__ASSERT(cpu_num == 1, "ESP32-S3 has two CPUs");

	__asm__ volatile("rsr.VECBASE %0\n\t" : "=r"(vb));

	sr.fn = fn;
	sr.arg = arg;
	sr.vecbase = vb;
	sr.alive = &alive_flag;
	appcpu_top = K_KERNEL_STACK_BUFFER(stack) + sz;
	start_rec = &sr;

	if (!cpus_active[0]) {
		ipi_init_pro_cpu();
		cpus_active[0] = true;
	}

	smp_start_core1(z_esp32s3_appcpu_asm_entry);

	while (!alive_flag) {
	}

	cpus_active[cpu_num] = true;
}

void arch_sched_directed_ipi(uint32_t cpu_bitmap)
{
	int self = esp_core_id();

	for (int cpu = 0; cpu < CONFIG_MP_MAX_NUM_CPUS; cpu++) {
		if (cpu != self && (cpu_bitmap & BIT(cpu)) != 0U && cpus_active[cpu]) {
			REG_WRITE(ipi_reg(cpu), SYSTEM_CPU_INTR_FROM_CPU_0);
		}
	}
}

void arch_sched_broadcast_ipi(void)
{
	arch_sched_directed_ipi(IPI_ALL_CPUS_MASK);
}

IRAM_ATTR bool arch_cpu_active(int cpu_num)
{
	return cpus_active[cpu_num];
}

#endif /* CONFIG_SMP */

/* AMP support */
#ifdef CONFIG_SOC_ENABLE_APPCPU

#include <bootloader_flash_priv.h>

#define sys_mmap   bootloader_mmap
#define sys_munmap bootloader_munmap

void esp_appcpu_start(void *entry_point)
{
	esp_cpu_unstall(1);

	if (!REG_GET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN)) {
		REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_CLKGATE_EN);
		REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RUNSTALL);
		REG_SET_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETTING);
		REG_CLR_BIT(SYSTEM_CORE_1_CONTROL_0_REG, SYSTEM_CONTROL_CORE_1_RESETTING);
	}

	esp_rom_ets_set_appcpu_boot_addr((void *)entry_point);

	esp_cpu_reset(1);
}

static int load_segment(uint32_t src_addr, uint32_t src_len, uint32_t dst_addr)
{
	const uint32_t *data = (const uint32_t *)sys_mmap(src_addr, src_len);

	if (!data) {
		ESP_EARLY_LOGE(TAG, "%s: mmap failed", __func__);
		return -1;
	}

	volatile uint32_t *dst = (volatile uint32_t *)dst_addr;

	for (int i = 0; i < src_len / 4; i++) {
		dst[i] = data[i];
	}

	sys_munmap(data);

	return 0;
}

int IRAM_ATTR esp_appcpu_image_load(unsigned int hdr_offset, unsigned int *entry_addr)
{
	const uint32_t fa_offset = PARTITION_OFFSET(slot0_appcpu_partition);
	const uint32_t fa_size = PARTITION_SIZE(slot0_appcpu_partition);
	const uint8_t fa_id = PARTITION_ID(slot0_appcpu_partition);

	if (entry_addr == NULL) {
		ESP_EARLY_LOGE(TAG, "Can't return the entry address. Aborting!");
		abort();
		return -1;
	}

	uint32_t mcuboot_header[8] = {0};
	esp_image_load_header_t image_header = {0};

	const uint32_t *data = (const uint32_t *)sys_mmap(fa_offset, 0x80);

	memcpy((void *)&mcuboot_header, data, sizeof(mcuboot_header));
	memcpy((void *)&image_header, data + (hdr_offset / sizeof(uint32_t)),
	       sizeof(esp_image_load_header_t));

	sys_munmap(data);

	if (image_header.header_magic == ESP_LOAD_HEADER_MAGIC) {
		ESP_EARLY_LOGI(TAG,
			"APPCPU image, area id: %d, offset: 0x%x, hdr.off: 0x%x, size: %d kB",
			fa_id, fa_offset, hdr_offset, fa_size / 1024);
	} else if ((image_header.header_magic & 0xff) == 0xE9) {
		ESP_EARLY_LOGE(TAG, "ESP image format is not supported");
		abort();
	} else {
		ESP_EARLY_LOGE(TAG, "Unknown or empty image detected. Aborting!");
		abort();
	}

	if (!esp_ptr_in_iram((void *)image_header.iram_dest_addr) ||
	    !esp_ptr_in_iram((void *)(image_header.iram_dest_addr + image_header.iram_size))) {
		ESP_EARLY_LOGE(TAG, "IRAM region in load header is not valid. Aborting");
		abort();
	}

	if (!esp_ptr_in_dram((void *)image_header.dram_dest_addr) ||
	    !esp_ptr_in_dram((void *)(image_header.dram_dest_addr + image_header.dram_size))) {
		ESP_EARLY_LOGE(TAG, "DRAM region in load header is not valid. Aborting");
		abort();
	}

	if (!esp_ptr_in_iram((void *)image_header.entry_addr)) {
		ESP_EARLY_LOGE(TAG, "Application entry point (%xh) is not in IRAM. Aborting",
			   image_header.entry_addr);
		abort();
	}

	ESP_EARLY_LOGI(TAG, "IRAM segment: paddr=%08xh, vaddr=%08xh, size=%05xh (%6d) load",
		   (fa_offset + image_header.iram_flash_offset), image_header.iram_dest_addr,
		   image_header.iram_size, image_header.iram_size);

	load_segment(fa_offset + image_header.iram_flash_offset, image_header.iram_size,
		     image_header.iram_dest_addr);

	ESP_EARLY_LOGI(TAG, "DRAM segment: paddr=%08xh, vaddr=%08xh, size=%05xh (%6d) load",
		   (fa_offset + image_header.dram_flash_offset), image_header.dram_dest_addr,
		   image_header.dram_size, image_header.dram_size);

	load_segment(fa_offset + image_header.dram_flash_offset, image_header.dram_size,
		     image_header.dram_dest_addr);

	ESP_EARLY_LOGI(TAG, "IROM segment: paddr=%08xh, vaddr=%08xh, size=%05xh (%6d) map",
		   (fa_offset + image_header.irom_flash_offset), image_header.irom_map_addr,
		   image_header.irom_size, image_header.irom_size);

	ESP_EARLY_LOGI(TAG, "DROM segment: paddr=%08xh, vaddr=%08xh, size=%05xh (%6d) map",
		   (fa_offset + image_header.drom_flash_offset), image_header.drom_map_addr,
		   image_header.drom_size, image_header.drom_size);

	struct rom_segments rom = {
		image_header.drom_map_addr,
		image_header.drom_flash_offset + fa_offset,
		image_header.drom_size,
		image_header.irom_map_addr,
		image_header.irom_flash_offset + fa_offset,
		image_header.irom_size,
	};

	map_rom_segments(1, &rom);

	ESP_EARLY_LOGI(TAG, "Application start=%xh\n\n", image_header.entry_addr);
	esp_rom_output_tx_wait_idle(0);

	assert(entry_addr != NULL);
	*entry_addr = image_header.entry_addr;

	return 0;
}

void esp_appcpu_image_stop(void)
{
	esp_cpu_stall(1);
}

void esp_appcpu_image_start(unsigned int hdr_offset)
{
	static int started;
	unsigned int entry_addr = 0;

	if (started) {
		printk("APPCPU already started.\r\n");
		return;
	}

	/* Input image meta header, output appcpu entry point */
	esp_appcpu_image_load(hdr_offset, &entry_addr);

	esp_appcpu_start((void *)entry_addr);
}

int esp_appcpu_init(void)
{
	/* Load APPCPU image using image header offset
	 * (skipping the MCUBoot header)
	 */
	esp_appcpu_image_start(0x20);

	return 0;
}

#if !defined(CONFIG_MCUBOOT)
extern int esp_appcpu_init(void);
SYS_INIT(esp_appcpu_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif

#endif /* CONFIG_SOC_ENABLE_APPCPU */
