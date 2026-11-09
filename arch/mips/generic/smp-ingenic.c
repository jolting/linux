// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMP support for Ingenic XBurst2 SoCs
 *
 * The core control unit (CCU) holds the secondary cores in reset and gives
 * them the address to start from. It has a mailbox per core: setting bits
 * in a core's mailbox raises CPU interrupt line 3 on that core, which is
 * the inter-processor interrupt. It also enables, per core, the lines of
 * the mailboxes, of the SoC interrupt controller (line 2) and of the OST
 * (line 4).
 *
 * Peripheral interrupts stay with the boot CPU: the other cores take only
 * their mailbox and their own OST timer.
 *
 * Register layout from Ingenic's X2600 SDK (Linux 6.6).
 */

#include <linux/bits.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/processor.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/cpu-info.h>
#include <asm/mipsregs.h>
#include <asm/smp-ops.h>

#include "smp-ingenic.h"

#define CCU_REG_CSRR		0x040	/* core reset: a set bit holds the core */
#define CCU_REG_PIMR		0x120	/* interrupt controller line enable */
#define CCU_REG_MIMR		0x160	/* mailbox line enable */
#define CCU_REG_OIMR		0x1a0	/* OST line enable */
#define CCU_REG_RER		0xf00	/* reset entry address */
#define CCU_REG_MBOX(core)	(0x1000 + 4 * (core))

unsigned long ingenic_smp_boot_sp;
unsigned long ingenic_smp_boot_gp;

static struct device_node *ccu_np;
static void __iomem *ccu;

/* Serialises the read-modify-write of the CCU registers and mailboxes */
static DEFINE_RAW_SPINLOCK(ccu_lock);

static void ccu_update_bits(unsigned int reg, u32 mask, u32 val)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&ccu_lock, flags);
	writel((readl(ccu + reg) & ~mask) | val, ccu + reg);
	raw_spin_unlock_irqrestore(&ccu_lock, flags);
}

static void ingenic_send_ipi_single(int cpu, unsigned int action)
{
	void __iomem *mbox = ccu + CCU_REG_MBOX(cpu_logical_map(cpu));
	unsigned long flags;

	raw_spin_lock_irqsave(&ccu_lock, flags);
	writel(readl(mbox) | action, mbox);
	raw_spin_unlock_irqrestore(&ccu_lock, flags);
}

static void ingenic_send_ipi_mask(const struct cpumask *mask,
				  unsigned int action)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		ingenic_send_ipi_single(cpu, action);
}

static irqreturn_t ingenic_ipi_interrupt(int irq, void *dev_id)
{
	void __iomem *mbox = ccu + CCU_REG_MBOX(get_ebase_cpunum());
	u32 action;

	raw_spin_lock(&ccu_lock);
	action = readl(mbox);
	writel(0, mbox);
	raw_spin_unlock(&ccu_lock);

	if (action & SMP_RESCHEDULE_YOURSELF)
		scheduler_ipi();

	if (action & SMP_CALL_FUNCTION)
		generic_smp_call_function_interrupt();

	return action ? IRQ_HANDLED : IRQ_NONE;
}

static void ingenic_enable_core_lines(unsigned int core, bool intc)
{
	writel(0, ccu + CCU_REG_MBOX(core));
	ccu_update_bits(CCU_REG_MIMR, BIT(core), BIT(core));
	ccu_update_bits(CCU_REG_OIMR, BIT(core), BIT(core));
	ccu_update_bits(CCU_REG_PIMR, BIT(core), intc ? BIT(core) : 0);
}

static void ingenic_init_secondary(void)
{
	ingenic_enable_core_lines(get_ebase_cpunum(), false);

	/* The mailbox and the OST; the SoC interrupt controller stays off */
	change_c0_status(ST0_IM, STATUSF_IP3 | STATUSF_IP4);
}

static void ingenic_smp_finish(void)
{
	local_irq_enable();
}

static int ingenic_boot_secondary(int cpu, struct task_struct *idle)
{
	unsigned int core = cpu_logical_map(cpu);
	unsigned long entry = (unsigned long)ingenic_smp_entry;

	ingenic_smp_boot_sp = __KSTK_TOS(idle);
	ingenic_smp_boot_gp = (unsigned long)task_thread_info(idle);

	/*
	 * The core starts with an empty cache: make sure what it reads
	 * before it caches KSEG0 is in memory.
	 */
	dma_cache_wback(entry, (unsigned long)ingenic_smp_entry_end - entry);
	dma_cache_wback((unsigned long)&ingenic_smp_boot_sp,
			sizeof(ingenic_smp_boot_sp));
	dma_cache_wback((unsigned long)&ingenic_smp_boot_gp,
			sizeof(ingenic_smp_boot_gp));

	writel(entry, ccu + CCU_REG_RER);
	ccu_update_bits(CCU_REG_CSRR, BIT(core), BIT(core));
	ccu_update_bits(CCU_REG_CSRR, BIT(core), 0);

	return 0;
}

static void __init ingenic_smp_setup(void)
{
	struct device_node *np;
	u32 others = 0;
	u64 hwid;

	init_cpu_possible(cpumask_of(0));
	init_cpu_present(cpumask_of(0));

	for_each_of_cpu_node(np) {
		hwid = of_get_cpu_hwid(np, 0);
		if (hwid == 0 || hwid >= nr_cpu_ids)
			continue;

		set_cpu_possible(hwid, true);
		set_cpu_present(hwid, true);
		__cpu_number_map[hwid] = hwid;
		__cpu_logical_map[hwid] = hwid;
		others |= BIT(hwid);
	}

	/* Whatever ran before may have left the other cores running */
	ccu_update_bits(CCU_REG_CSRR, others, others);

	pr_info("Ingenic SMP: %u CPUs\n", num_possible_cpus());
}

static void __init ingenic_prepare_cpus(unsigned int max_cpus)
{
	unsigned int irq;
	int err;

	irq = irq_of_parse_and_map(ccu_np, 0);
	if (!irq) {
		pr_err("Ingenic SMP: no IPI interrupt\n");
		goto err_up;
	}

	err = request_irq(irq, ingenic_ipi_interrupt,
			  IRQF_PERCPU | IRQF_NO_SUSPEND | IRQF_NO_THREAD,
			  "ipi", NULL);
	if (err) {
		pr_err("Ingenic SMP: cannot request the IPI: %d\n", err);
		goto err_up;
	}

	ingenic_enable_core_lines(0, true);
	return;

err_up:
	init_cpu_possible(cpumask_of(0));
	init_cpu_present(cpumask_of(0));
}

static const struct plat_smp_ops ingenic_smp_ops = {
	.send_ipi_single	= ingenic_send_ipi_single,
	.send_ipi_mask		= ingenic_send_ipi_mask,
	.init_secondary		= ingenic_init_secondary,
	.smp_finish		= ingenic_smp_finish,
	.boot_secondary		= ingenic_boot_secondary,
	.smp_setup		= ingenic_smp_setup,
	.prepare_cpus		= ingenic_prepare_cpus,
};

int __init register_ingenic_smp_ops(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "ingenic,x2600-ccu");
	if (!np)
		return -ENODEV;

	/* Secondary cores are numbered from the boot CPU, core 0 */
	if (get_ebase_cpunum() != 0) {
		pr_warn("Ingenic SMP: booted on core %u, staying UP\n",
			get_ebase_cpunum());
		goto err_put;
	}

	ccu = of_iomap(np, 0);
	if (!ccu)
		goto err_put;

	ccu_np = np;
	register_smp_ops(&ingenic_smp_ops);

	return 0;

err_put:
	of_node_put(np);
	return -ENODEV;
}
