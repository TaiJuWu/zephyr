/*
 * Copyright (c) 2026 TaiJu Wu <tjwu1217@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/arch/riscv/csr.h>
#include <zephyr/drivers/interrupt_controller/intc_esp32.h>
#include <zephyr/drivers/pm_cpu_ops.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/sys/atomic.h>
#include <kernel_arch_interface.h>
#include <ipi.h>

#include <soc.h>
#include <soc/interrupts.h>
#include <soc/hp_system_reg.h>
#include <soc/hp_sys_clkrst_reg.h>
#include <hal/crosscore_int_ll.h>
#include <hal/interrupt_clic_ll.h>
#include <riscv/interrupt.h>
#include <esp_cpu.h>
#include <esp_rom_sys.h>
#include <rom/ets_sys.h>

#define IPI_SCHED     0
#define IPI_FPU_FLUSH 1

static atomic_val_t cpu_pending_ipi[CONFIG_MP_MAX_NUM_CPUS];

extern void esp32p4_appcpu_entry(void);
extern uint32_t _vector_table;
extern uint32_t _mtvt_table;

static void IRAM_ATTR send_ipi(unsigned int cpu, int reason)
{
	atomic_set_bit(&cpu_pending_ipi[cpu], reason);
	crosscore_int_ll_trigger_interrupt(_kernel.cpus[cpu].arch.hartid);
}

void arch_sched_directed_ipi(uint32_t cpu_bitmap)
{
	unsigned int key = arch_irq_lock();
	unsigned int id = _current_cpu->id;

	for (unsigned int i = 0; i < arch_num_cpus(); i++) {
		if ((i != id) && _kernel.cpus[i].arch.online && ((cpu_bitmap & BIT(i)) != 0)) {
			send_ipi(i, IPI_SCHED);
		}
	}

	arch_irq_unlock(key);
}

#ifdef CONFIG_FPU_SHARING
void arch_flush_fpu_ipi(unsigned int cpu)
{
	send_ipi(cpu, IPI_FPU_FLUSH);
}

void arch_spin_relax(void)
{
	atomic_val_t *pending_ipi = &cpu_pending_ipi[_current_cpu->id];

	if (atomic_test_and_clear_bit(pending_ipi, IPI_FPU_FLUSH)) {
		z_riscv_fpu_flush_thread(_current_cpu->arch.fpu_owner);
	}
}
#endif /* CONFIG_FPU_SHARING */

static void IRAM_ATTR ipi_handler(void *arg)
{
	ARG_UNUSED(arg);

	crosscore_int_ll_clear_interrupt(esp_core_id());

	atomic_val_t pending_ipi = atomic_clear(&cpu_pending_ipi[_current_cpu->id]);

	if (pending_ipi & ATOMIC_MASK(IPI_SCHED)) {
		z_sched_ipi();
	}
#ifdef CONFIG_FPU_SHARING
	if (pending_ipi & ATOMIC_MASK(IPI_FPU_FLUSH)) {
		csr_clear(mstatus, MSTATUS_IEN);
		arch_flush_local_fpu();
	}
#endif
}

/*
 * Each core takes its IPI on its own FROM_CPU source. Both are routed to the
 * same CPU interrupt line, since the ISR table is shared by the cores: CPU0
 * allocates the line, and the app CPU only routes and enables it, as it has
 * no thread yet to run the allocator's irq_lock() on.
 */
#define IPI_LEVEL 1

static int ipi_intno = -1;

int arch_smp_init(void)
{
	intr_handle_t handle;
	int ret;

	crosscore_int_ll_clear_interrupt(0);

	ret = esp_intr_alloc(ETS_FROM_CPU_INTR0_SOURCE, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL1,
			     ipi_handler, NULL, &handle);
	if (ret != 0) {
		return ret;
	}

	ipi_intno = esp_intr_get_intno(handle);

	return 0;
}

static void ipi_init_app_cpu(void)
{
	__ASSERT_NO_MSG(ipi_intno >= 0);

	crosscore_int_ll_clear_interrupt(1);
	esp_rom_route_intr_matrix(1, ETS_FROM_CPU_INTR1_SOURCE, ipi_intno);
	esp_cpu_intr_set_priority(ipi_intno, IPI_LEVEL);
	esp_cpu_intr_set_type(ipi_intno, ESP_CPU_INTR_TYPE_LEVEL);
	interrupt_clic_ll_set_vectored(ipi_intno + CLIC_EXT_INTR_NUM_OFFSET, true);
	esp_cpu_intr_enable(BIT(ipi_intno));
}

void soc_per_core_init_hook(void)
{
	if (esp_core_id() == 0) {
		return;
	}

	/* The app CPU enters from the ROM with the ROM's trap setup. */
	esp_cpu_intr_set_ivt_addr(&_vector_table);
	esp_cpu_intr_set_mtvt_addr(&_mtvt_table);

	for (int i = 0; i < ETS_MAX_INTR_SOURCE; i++) {
		interrupt_clic_ll_route(1, i, ETS_INVALID_INUM);
	}
	for (int i = 0; i < 32; i++) {
		esprv_int_set_vectored(i, true);
	}

	ipi_init_app_cpu();
}

/*
 * Called by arch_cpu_start(). The entry point it passes is the generic one,
 * which the ESP32-P4 boot path does not use: the app CPU starts at
 * esp32p4_appcpu_entry instead and joins the generic handshake there.
 */
int pm_cpu_on(unsigned long cpuid, uintptr_t entry_point)
{
	ARG_UNUSED(entry_point);

	if (cpuid != 1) {
		return -EINVAL;
	}

	/* An idle core in WFI must keep its clock, or it stops taking IPIs. */
	REG_SET_BIT(HP_SYSTEM_CPU_WAITI_CONF_REG, HP_SYSTEM_CPU_WAIT_MODE_FORCE_ON);

	esp_cpu_unstall(1);
	REG_SET_BIT(HP_SYS_CLKRST_SOC_CLK_CTRL0_REG, HP_SYS_CLKRST_REG_CORE1_CPU_CLK_EN);
	REG_CLR_BIT(HP_SYS_CLKRST_HP_RST_EN0_REG, HP_SYS_CLKRST_REG_RST_EN_CORE1_GLOBAL);
	ets_set_appcpu_boot_addr((uint32_t)esp32p4_appcpu_entry);

	return 0;
}

/*
 * A flash write or erase suspends the cache shared by both cores, so the
 * other core must not fetch from flash meanwhile. Stall it in hardware.
 */
void IRAM_ATTR soc_mp_pause_others(void)
{
	if (arch_num_cpus() > 1 && _kernel.cpus[1].arch.online) {
		esp_cpu_stall(esp_core_id() ^ 1);
	}
}

void IRAM_ATTR soc_mp_resume_others(void)
{
	if (arch_num_cpus() > 1 && _kernel.cpus[1].arch.online) {
		esp_cpu_unstall(esp_core_id() ^ 1);
	}
}
