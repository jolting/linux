/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __MIPS_GENERIC_SMP_INGENIC_H
#define __MIPS_GENERIC_SMP_INGENIC_H

#include <linux/linkage.h>

/* Where the CCU starts a secondary core, in smp-ingenic-entry.S */
asmlinkage void ingenic_smp_entry(void);
extern char ingenic_smp_entry_end[];

/* The stack and thread_info ingenic_smp_entry() starts the core with */
extern unsigned long ingenic_smp_boot_sp;
extern unsigned long ingenic_smp_boot_gp;

#endif /* __MIPS_GENERIC_SMP_INGENIC_H */
