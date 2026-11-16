/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_ALTERNATIVE_H
#define __ASM_ALTERNATIVE_H

#include <linux/stringify.h>
#include <asm/asm.h>

#define MIPS_ALT_RW_LLB		0
#define MIPS_ALT_NFEATURES	1

#ifndef __ASSEMBLER__

#include <linux/init.h>
#include <linux/types.h>
#include <linux/kconfig.h>

struct alt_instr {
	unsigned long orig;	/* original instruction */
	unsigned long alt;	/* replacement instruction */
	u16 feature;		/* MIPS_ALT_* */
	u8  orig_len;		/* size of original instruction(s) */
	u8  alt_len;		/* size of new instruction(s), == orig_len */
};

void __init apply_alternatives_all(void);
void apply_alternatives_module(void *start, size_t length);
void verify_local_cpu_alternatives(void);

#define ALTINSTR_ENTRY(feature)						      \
	" .align " __stringify(PTRLOG) "\n"				      \
	" " __stringify(PTR_WD) " 661b\n"	/* label           */	      \
	" " __stringify(PTR_WD) " 663f\n"	/* new instruction */	      \
	" .hword " __stringify(feature) "\n"	/* feature         */	      \
	" .byte 662b-661b\n"			/* source len      */	      \
	" .byte 664f-663f\n"			/* replacement len */

/*
 * alternative assembly primitive: if either .org directive fails,
 * oldinstr and newinstr don't have the same length.
 *
 * newinstr is copied as is, so it must not contain PC-relative
 * instructions (branches, j/jal, R6 *pc loads).
 */
#define __ALTERNATIVE_CFG(oldinstr, newinstr, feature, cfg_enabled)	\
	".if "__stringify(cfg_enabled)" == 1\n"				\
	"661:\n\t"							\
	oldinstr "\n"							\
	"662:\n"							\
	".pushsection .altinstructions,\"a\"\n"				\
	ALTINSTR_ENTRY(feature)						\
	".popsection\n"							\
	".subsection 1\n"						\
	"663:\n\t"							\
	newinstr "\n"							\
	"664:\n\t"							\
	".org	. - (664b-663b) + (662b-661b)\n\t"			\
	".org	. - (662b-661b) + (664b-663b)\n\t"			\
	".previous\n"							\
	".endif\n"

#define _ALTERNATIVE_CFG(oldinstr, newinstr, feature, cfg, ...)	\
	__ALTERNATIVE_CFG(oldinstr, newinstr, feature, IS_ENABLED(cfg))

#else

.macro altinstruction_entry orig, alt, feature, orig_len, alt_len
	.align	PTRLOG
	PTR_WD	\orig
	PTR_WD	\alt
	.hword	\feature
	.byte	\orig_len
	.byte	\alt_len
.endm

.macro alternative_insn insn1, insn2, feature, enable = 1
	.if \enable
661:	\insn1
662:	.pushsection .altinstructions, "a"
	altinstruction_entry 661b, 663f, \feature, 662b-661b, 664f-663f
	.popsection
	.subsection 1
663:	\insn2
664:	.org	. - (664b-663b) + (662b-661b)
	.org	. - (662b-661b) + (664b-663b)
	.previous
	.endif
.endm

#endif  /*  __ASSEMBLER__  */

/*
 * Usage: asm(ALTERNATIVE(oldinstr, newinstr, feature));
 *
 * Usage: asm(ALTERNATIVE(oldinstr, newinstr, feature, CONFIG_FOO));
 * N.B. If CONFIG_FOO is specified, but not selected, the whole block
 *      will be omitted, including oldinstr.
 */
#define ALTERNATIVE(oldinstr, newinstr, ...)   \
	_ALTERNATIVE_CFG(oldinstr, newinstr, __VA_ARGS__, 1)

#endif /* __ASM_ALTERNATIVE_H */
