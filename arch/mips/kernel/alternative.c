// SPDX-License-Identifier: GPL-2.0-only
/*
 * alternative runtime patching
 * inspired by the arm64 and x86 version
 */

#define pr_fmt(fmt) "alternatives: " fmt

#include <linux/bitmap.h>
#include <linux/cacheflush.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/smp.h>
#include <linux/stop_machine.h>
#include <linux/string.h>

#include <asm/alternative.h>
#include <asm/cpu-info.h>
#include <asm/cpu.h>

extern struct alt_instr __alt_instructions[], __alt_instructions_end[];

/* Features patched in: every CPU must have them */
static DECLARE_BITMAP(applied_alternatives, MIPS_ALT_NFEATURES);

static int all_alternatives_applied;

struct alt_region {
	struct alt_instr *begin;
	struct alt_instr *end;
};

static bool cpu_has_alt_feature(unsigned int cpu, u16 feature)
{
	const struct cpuinfo_mips *c = &cpu_data[cpu];

	switch (feature) {
	case MIPS_ALT_RW_LLB:
		return c->options & MIPS_CPU_RW_LLB;
	}
	return false;
}

static bool system_has_alt_feature(u16 feature)
{
	unsigned int cpu;

	for_each_online_cpu(cpu)
		if (!cpu_has_alt_feature(cpu, feature))
			return false;
	return true;
}

static void __apply_alternatives(const struct alt_region *region,
				 bool is_module, unsigned long *feature_mask)
{
	struct alt_instr *alt;

	for (alt = region->begin; alt < region->end; alt++) {
		if (alt->feature >= MIPS_ALT_NFEATURES ||
		    !test_bit(alt->feature, feature_mask))
			continue;

		if (WARN_ON(alt->alt_len != alt->orig_len))
			continue;

		memcpy((void *)alt->orig, (void *)alt->alt, alt->orig_len);

		if (is_module)
			flush_icache_range(alt->orig, alt->orig + alt->orig_len);
		else
			local_flush_icache_range(alt->orig,
						 alt->orig + alt->orig_len);
	}
}

/* Drop the old instructions from this CPU's instruction cache */
static void __init clean_local_icache(const struct alt_region *region)
{
	struct alt_instr *alt;

	for (alt = region->begin; alt < region->end; alt++)
		if (alt->feature < MIPS_ALT_NFEATURES &&
		    test_bit(alt->feature, applied_alternatives))
			local_flush_icache_range(alt->orig,
						 alt->orig + alt->orig_len);
}

static int __init __apply_alternatives_multi_stop(void *unused)
{
	struct alt_region region = {
		.begin	= __alt_instructions,
		.end	= __alt_instructions_end,
	};

	/* We always have a CPU 0 at this point (__init) */
	if (smp_processor_id()) {
		while (!READ_ONCE(all_alternatives_applied))
			cpu_relax();
		/* applied_alternatives before the code patched for it */
		smp_rmb();
		clean_local_icache(&region);
	} else {
		DECLARE_BITMAP(features, MIPS_ALT_NFEATURES);
		u16 f;

		bitmap_zero(features, MIPS_ALT_NFEATURES);
		for (f = 0; f < MIPS_ALT_NFEATURES; f++)
			if (system_has_alt_feature(f))
				__set_bit(f, features);

		__apply_alternatives(&region, false, features);
		bitmap_copy(applied_alternatives, features, MIPS_ALT_NFEATURES);

		/* the patched code before the flag the others wait for */
		smp_wmb();
		WRITE_ONCE(all_alternatives_applied, 1);
	}

	return 0;
}

void __init apply_alternatives_all(void)
{
	/* better not try code patching on a live SMP system */
	stop_machine(__apply_alternatives_multi_stop, NULL, cpu_online_mask);
}

void apply_alternatives_module(void *start, size_t length)
{
	struct alt_region region = {
		.begin	= start,
		.end	= start + length,
	};

	__apply_alternatives(&region, true, applied_alternatives);
}

/* A CPU brought up later must have every feature patched in */
void verify_local_cpu_alternatives(void)
{
	unsigned int cpu = smp_processor_id();
	u16 f;

	if (!READ_ONCE(all_alternatives_applied))
		return;

	for (f = 0; f < MIPS_ALT_NFEATURES; f++) {
		if (!test_bit(f, applied_alternatives) ||
		    cpu_has_alt_feature(cpu, f))
			continue;

		pr_crit("CPU%u: lacks feature %u in use, not starting\n", cpu, f);
		local_irq_disable();
		while (1)
			cpu_relax();
	}
}
