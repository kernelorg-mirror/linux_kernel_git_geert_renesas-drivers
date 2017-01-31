#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/types.h>

#include <linux/tictoc.h>
#include <linux/dma-debug.h>

static u32 tic_start;
static bool div64;

static unsigned int mhz;

static inline u32 get_pmucycles(void)
{
	u32 count;

#ifdef CONFIG_ARM64
	asm volatile("mrs %0, pmccntr_el0" : "=r" (count));
#else
	asm volatile("mrc p15, 0, %0, c9, c13, 0\n\t" : "=r" (count));
#endif
	return count;
}

#define PMINTENCLR_CLEAR	BIT(31)	/* Overflow Interrupt Clear */

#define PMCR_ENABLE		BIT(0)	/* Enable all counters */
#define PMCR_EVCNTR_RESET	BIT(1)	/* Event Counter Reset */
#define PMCR_CCNTR_RESET	BIT(2)	/* Clock Counter Reset */
#define PMCR_DIVIDER		BIT(3)	/* Clock Divider (/64) */

#define PMCNTENSET_ENABLE	BIT(31)	/* Cycle Counter Enable Set */

#define PMOVSR_ENABLE		BIT(31)	/* Cycle Counter Overflow Flag */

#define PMU_CYCLE_COUNTER	0x1f

static void tic_setup(u32 pmcr)
{
	u32 counters = BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4);
	div64 = !!pmcr;

#ifdef CONFIG_ARM64
	/* PMINTENCLR - Interrupt Enable Clear Register */
	/* Disable interrupt request for all counters */
	asm volatile("msr pmintenclr_el1, %0\n\t"
		     : : "r" (PMINTENCLR_CLEAR | counters));

	/* PMCR - Performance Monitor Control Register */
	/* Reset and enable counters */
	pmcr |= PMCR_ENABLE | PMCR_EVCNTR_RESET | PMCR_CCNTR_RESET;
	asm volatile("msr pmcr_el0, %0\n\t" : : "r" (pmcr));

	/* PMCNTENSET - Count Enable Set Register */
	/* Enable all possible counters */
	asm volatile("msr pmcntenset_el0, %0\n\t"
		     : : "r" (PMCNTENSET_ENABLE | counters));

	/* PMOVSR - Overflow Flag Status Register */
	/* Clear counters */
	asm volatile("msr pmovsclr_el0, %0\n\t"
		     : : "r" (PMOVSR_ENABLE | counters));

	/* PMSELR - Event Counter Selection Register */
	/* Select cycles counter */
	asm volatile("msr pmselr_el0, %0\n\t" : : "r" (PMU_CYCLE_COUNTER));
#else
	/* PMINTENCLR - Interrupt Enable Clear Register */
	/* Disable interrupt request for all counters */
	asm volatile("mcr p15, 0, %0, c9, c14, 2\n\t"
		     : : "r" (PMINTENCLR_CLEAR | counters));

	/* PMCR - Performance Monitor Control Register */
	/* Reset and enable counters */
	pmcr |= PMCR_ENABLE | PMCR_EVCNTR_RESET | PMCR_CCNTR_RESET;
	asm volatile("mcr p15, 0, %0, c9, c12, 0\n\t"
		     : : "r" (pmcr));

	/* PMCNTENSET - Count Enable Set Register */
	/* Enable all possible counters */
	asm volatile("mcr p15, 0, %0, c9, c12, 1\n\t"
		     : : "r" (PMCNTENSET_ENABLE | counters));

	/* PMOVSR - Overflow Flag Status Register */
	/* Clear counters */
	asm volatile("mcr p15, 0, %0, c9, c12, 3\n\t"
		     : : "r" (PMOVSR_ENABLE | counters));

	/* PMSELR - Event Counter Selection Register */
	/* Select cycles counter */
	asm volatile("mcr p15, 0, %0, c9, c12, 5\n\t"
		     : : "r" (PMU_CYCLE_COUNTER));
#endif
}

static unsigned int ticks_to_ns(unsigned int n)
{
	switch (mhz) {
	case 400:
		if (div64)
			return n * 160;
		return n < 0x10000 ? n * 5 / 2 : n / 2 * 5;

	case 533:
		if (div64)
			return n * 120;
		return n < 0x10000? n * 15 / 8 : n / 8 * 15;

	case 800:
		if (div64)
			return n * 80;
		return n < 0x10000 ? n * 5 / 4 : n / 4 * 5;

	case 1000:
		return div64 ? n * 64 : n;

	case 1200:
		if (n < 0x10000)
			return div64 ? n * 160 / 3 : n * 5 / 6;
		return div64 ? n / 3 * 160 : n / 6 * 5;

	case 1300:
		if (n < 0x10000)
			return div64 ? n * 640 / 13 : n * 10 / 13;
		return div64 ? n / 13 * 640 : n / 13 * 10;

	case 1500:
		if (n < 0x10000)
			return div64 ? n * 128 / 3 : n * 2 / 3;
		return div64 ? n / 3 * 128 : n / 3 * 2;

	default:
		return 0;
	}
}

void tic(void)
{
	/* Disable cycles counter divide */
	tic_setup(0);
	tic_start = get_pmucycles();
}

void tic64(void)
{
	/* Enable cycles counter divide */
	tic_setup(PMCR_DIVIDER);
	tic_start = get_pmucycles();
}

unsigned int toc(void)
{
	return get_pmucycles() - tic_start;
}

unsigned int toc_ns(void)
{
	return ticks_to_ns(toc());
}

u64 tictoc_ktime_get(void)
{
	static u64 ns_time;

	if (!ns_time)
		ns_time = 1;
	else
		ns_time += ticks_to_ns(get_pmucycles() - tic_start);

	/* Disable cycles counter divide */
	tic_setup(0);
	tic_start = get_pmucycles();

	return ns_time;
}

static const struct of_device_id renesas_socs[] __initconst = {
#ifdef CONFIG_ARCH_EMEV2
	// 533 MHz Cortex-A9
	{ .compatible = "renesas,emev2",	.data = (void *)533 },
#endif
#ifdef CONFIG_ARCH_R7S72100
	// 400 MHz Cortex-A9
	{ .compatible = "renesas,r7s72100",	.data = (void *)400 },
#endif
#ifdef CONFIG_ARCH_R8A73A4
	// 1 GHz Cortex-A15
	{ .compatible = "renesas,r8a73a4",	.data = (void *)1000 },
#endif
#ifdef CONFIG_ARCH_R8A7740
	// 800 MHz Cortex-A9
	{ .compatible = "renesas,r8a7740",	.data = (void *)800 },
#endif
#ifdef CONFIG_ARCH_R8A7743
	// 1.5 GHz Cortex-A15
	{ .compatible = "renesas,r8a7743",	.data = (void *)1500 },
#endif
#ifdef CONFIG_ARCH_R8A7745
	// 1.5 GHz Cortex-A7
	{ .compatible = "renesas,r8a7743",	.data = (void *)1500 },
#endif
#ifdef CONFIG_ARCH_R8A7778
	// 800 MHz Cortex-A9
	{ .compatible = "renesas,r8a7778",	.data = (void *)800 },
#endif
#ifdef CONFIG_ARCH_R8A7779
	// 1 GHz Cortex-A9
	{ .compatible = "renesas,r8a7779",	.data = (void *)1000 },
#endif
#ifdef CONFIG_ARCH_R8A7790
	// 1.3 GHz Cortex-A15
	{ .compatible = "renesas,r8a7790",	.data = (void *)1300 },
#endif
#ifdef CONFIG_ARCH_R8A7791
	// 1.5 GHz Cortex-A15
	{ .compatible = "renesas,r8a7791",	.data = (void *)1500 },
#endif
#ifdef CONFIG_ARCH_R8A7792
	// 1.5 GHz Cortex-A15
	{ .compatible = "renesas,r8a7792",	.data = (void *)1500 },
#endif
#ifdef CONFIG_ARCH_R8A7793
	// 1.5 GHz Cortex-A15
	{ .compatible = "renesas,r8a7793",	.data = (void *)1500 },
#endif
#ifdef CONFIG_ARCH_R8A7794
	// 1.5 GHz Cortex-A7
	{ .compatible = "renesas,r8a7794",	.data = (void *)1500 },
#endif
#ifdef CONFIG_ARCH_R8A7795
	// 1.5 GHz Cortex-A57
	{ .compatible = "renesas,r8a7795",	.data = (void *)1500 },
#endif
#ifdef CONFIG_ARCH_R8A7796
	// 1.5 GHz Cortex-A57
	{ .compatible = "renesas,r8a7796",	.data = (void *)1500 },
#endif
#ifdef CONFIG_ARCH_SH73A0
	// 1.2 GHz Cortex-A9
	{ .compatible = "renesas,sh73a0",	.data = (void *)1200 },
#endif
	{ /* sentinel */ }
};

static int __init tic_toc_setup(void)
{
	const struct of_device_id *match;
	struct device_node *np;

	np = of_find_matching_node_and_match(NULL, renesas_socs, &match);
	if (!np) {
		pr_warn("%s: Unknown Renesas SoC\n", __func__);
		return -ENODEV;
	}

	mhz = (uintptr_t)match->data;
	pr_info("Renesas SoC at %u MHz\n", mhz);
#define PREALLOC_DMA_DEBUG_ENTRIES	4096
	dma_debug_init(PREALLOC_DMA_DEBUG_ENTRIES);
	return 0;
}
arch_initcall(tic_toc_setup);

#if 0
#include <linux/delay.h>

static __noreturn int __init loop(void)
{
	pr_info("%s\n", __func__);
	while (1) {
		tic();
		mdelay(1000);
		pr_info("mdelay(1000) took %u cycles\n", toc());
		tic();
		mdelay(1000);
		pr_info("mdelay(1000) took %u ns\n", toc_ns());
		tic64();
		mdelay(1000);
		pr_info("mdelay(1000) took %u cycles (x64)\n", toc());
		tic64();
		mdelay(1000);
		pr_info("mdelay(1000) took %u ns (x64)\n", toc_ns());
	}
}
late_initcall(loop);
#endif
