/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Entry point of the secondary cores of Ingenic XBurst2 SoCs
 *
 * The core control unit (CCU) starts a core released from reset at the
 * address in its reset entry register, with the CP0 state of a reset.
 */

#include <asm/addrspace.h>
#include <asm/asm.h>
#include <asm/mipsregs.h>
#include <asm/regdef.h>

	.set	noreorder
	.set	mips32r2

	.balign	64
LEAF(ingenic_smp_entry)
	/* Cache KSEG0 like the boot CPU */
	mfc0	t0, CP0_CONFIG
	ori	t0, CONF_CM_CMASK
	xori	t0, CONF_CM_CMASK ^ CONF_CM_CACHABLE_NONCOHERENT
	mtc0	t0, CP0_CONFIG

	/* No pending software interrupts; kernel mode, BEV and ERL clear */
	mtc0	zero, CP0_CAUSE
	li	t0, ST0_CU0
	mtc0	t0, CP0_STATUS
	ehb

	PTR_LA	t0, ingenic_smp_boot_sp
	PTR_L	sp, 0(t0)
	PTR_LA	t0, ingenic_smp_boot_gp
	PTR_L	gp, 0(t0)

	PTR_LA	t9, smp_bootstrap
	jr.hb	t9
	 nop
	.globl	ingenic_smp_entry_end
ingenic_smp_entry_end:
	END(ingenic_smp_entry)
