// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2001 MontaVista Software Inc.
 * Author: Matt Porter <mporter@mvista.com>
 *
 * Copyright (C) 2009 Lemote, Inc.
 * Author: Wu Zhangjin <wuzhangjin@gmail.com>
 */

#define DISABLE_BRANCH_PROFILING

#define __NO_FORTIFY
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/libfdt.h>

#include <asm/addrspace.h>
#include <linux/unaligned.h>
#include <asm-generic/vmlinux.lds.h>

#include "decompress.h"

/*
 * These two variables specify the free mem region
 * that can be used for temporary malloc area
 */
unsigned long free_mem_ptr;
unsigned long free_mem_end_ptr;

#if (defined(CONFIG_CPU_MIPSR1) || defined(CONFIG_CPU_MIPSR2) ||	\
	defined(CONFIG_CPU_MIPSR5) || defined(CONFIG_CPU_MIPSR6)) &&	\
	!defined(CONFIG_CPU_CAVIUM_OCTEON)
#define CONF_M		BIT(31)		/* Config1 is implemented */

/* Index operations over a whole L1 cache, its geometry from Config1 */
#define CACHE_BLAST(op, sets_shift, line_shift, ways_shift)		\
do {									\
	unsigned int line = (config1 >> (line_shift)) & 7;		\
	unsigned long addr, end;					\
									\
	if (line) {							\
		line = 2 << line;					\
		end = CKSEG0 + (64 << ((config1 >> (sets_shift)) & 7)) *	\
		      line * (((config1 >> (ways_shift)) & 7) + 1);	\
		for (addr = CKSEG0; addr < end; addr += line)		\
			__asm__ __volatile__("cache %0, 0(%1)"		\
					     : : "i" (op), "r" (addr));	\
	}								\
} while (0)

/*
 * The kernel was just written through the data cache, and the instruction
 * cache may hold whatever code was at its addresses before, from a boot
 * loader or a previous kernel. Write back the L1 data cache and invalidate
 * the L1 instruction cache so that the kernel runs the code just written.
 */
static void flush_l1_caches(void)
{
	unsigned int config, config1;

	__asm__ __volatile__("mfc0 %0, $16, 0" : "=r" (config));
	if (!(config & CONF_M))
		return;
	__asm__ __volatile__("mfc0 %0, $16, 1" : "=r" (config1));

	CACHE_BLAST(0x01, 13, 10, 7);	/* Index_Writeback_Inv_D */
	__asm__ __volatile__("sync" : : : "memory");
	CACHE_BLAST(0x00, 22, 19, 16);	/* Index_Invalidate_I */
	__asm__ __volatile__("ehb" : : : "memory");
}
#else
static inline void flush_l1_caches(void) { }
#endif

void error(char *x)
{
	puts("\n\n");
	puts(x);
	puts("\n\n -- System halted");

	while (1)
		;	/* Halt */
}

/* activate the code for pre-boot environment */
#define STATIC static

#ifdef CONFIG_KERNEL_GZIP
#include "../../../../lib/decompress_inflate.c"
#endif

#ifdef CONFIG_KERNEL_BZIP2
#include "../../../../lib/decompress_bunzip2.c"
#endif

#ifdef CONFIG_KERNEL_LZ4
#include "../../../../lib/decompress_unlz4.c"
#endif

#ifdef CONFIG_KERNEL_LZMA
#include "../../../../lib/decompress_unlzma.c"
#endif

#ifdef CONFIG_KERNEL_LZO
#include "../../../../lib/decompress_unlzo.c"
#endif

#ifdef CONFIG_KERNEL_XZ
#include "../../../../lib/decompress_unxz.c"
#endif

#ifdef CONFIG_KERNEL_ZSTD
#include "../../../../lib/decompress_unzstd.c"
#endif

const unsigned long __stack_chk_guard = 0x000a0dff;

void __stack_chk_fail(void)
{
	error("stack-protector: Kernel stack is corrupted\n");
}

void decompress_kernel(unsigned long boot_heap_start)
{
	unsigned long zimage_start, zimage_size;

	zimage_start = (unsigned long)(__image_begin);
	zimage_size = (unsigned long)(__image_end) -
	    (unsigned long)(__image_begin);

	puts("zimage at:     ");
	puthex(zimage_start);
	puts(" ");
	puthex(zimage_size + zimage_start);
	puts("\n");

	/* This area are prepared for mallocing when decompressing */
	free_mem_ptr = boot_heap_start;
	free_mem_end_ptr = boot_heap_start + BOOT_HEAP_SIZE;

	/* Display standard Linux/MIPS boot prompt */
	puts("Uncompressing Linux at load address ");
	puthex(VMLINUX_LOAD_ADDRESS_ULL);
	puts("\n");

	/* Decompress the kernel with according algorithm */
	__decompress((char *)zimage_start, zimage_size, 0, 0,
		   (void *)VMLINUX_LOAD_ADDRESS_ULL, 0, 0, error);

	if (IS_ENABLED(CONFIG_MIPS_RAW_APPENDED_DTB) &&
	    fdt_magic((void *)&__appended_dtb) == FDT_MAGIC) {
		unsigned int image_size, dtb_size;

		dtb_size = fdt_totalsize((void *)&__appended_dtb);

		/* last four bytes is always image size in little endian */
		image_size = get_unaligned_le32((void *)__image_end - 4);

		/* The device tree's address must be properly aligned  */
		image_size = ALIGN(image_size, STRUCT_ALIGNMENT);

		puts("Copy device tree to address  ");
		puthex(VMLINUX_LOAD_ADDRESS_ULL + image_size);
		puts("\n");

		/* copy dtb to where the booted kernel will expect it */
		memcpy((void *)VMLINUX_LOAD_ADDRESS_ULL + image_size,
		       __appended_dtb, dtb_size);
	}

	flush_l1_caches();
	puts("Now, booting the kernel...\n");
}
