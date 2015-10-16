/*
 * r8a7791 Clock Pulse Generator / Module Standby and Software Reset
 *
 * Copyright (C) 2015 Glider bvba
 *
 * Based on clk-rcar-gen2.c
 *
 * Copyright (C) 2013 Ideas On Board SPRL
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

#include <linux/bug.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/r8a7791-cpg-mssr.h>

#include "clk-cpg-mssr.h"


enum clk_ids {
	/* Core Clock Outputs exported to DT */
	LAST_DT_CORE_CLK = R8A7791_CLK_OSC,

	/* External Input Clocks */
	CLK_EXTAL,
	CLK_USB_EXTAL,

	/* Internal Core Clocks */
	CLK_MAIN,
	CLK_PLL0,
	CLK_PLL1,
	CLK_PLL3,
	CLK_PLL1_DIV2,

	/* Module Clocks */
	MOD_CLK_BASE
};

enum r8a7791_clk_types {
	CLK_TYPE_GEN2_MAIN = CLK_TYPE_CUSTOM,
	CLK_TYPE_GEN2_PLL0,
	CLK_TYPE_GEN2_PLL1,
	CLK_TYPE_GEN2_PLL3,
	CLK_TYPE_GEN2_Z,
	CLK_TYPE_GEN2_LB,
	CLK_TYPE_GEN2_ADSP,
	CLK_TYPE_GEN2_SDH,
	CLK_TYPE_GEN2_SD0,
	CLK_TYPE_GEN2_QSPI,
	CLK_TYPE_GEN2_RCAN,
};

static const struct cpg_core_clk r8a7791_core_clks[] __initconst = {
	/* External Clock Inputs */
	DEF_INPUT("extal", CLK_EXTAL),
	DEF_INPUT("usb_extal", CLK_USB_EXTAL),

	/* Internal Core Clocks */
	DEF_BASE(".main",       CLK_MAIN, CLK_TYPE_GEN2_MAIN, CLK_EXTAL),
	DEF_BASE(".pll0",       CLK_PLL0, CLK_TYPE_GEN2_PLL0, CLK_MAIN),
	DEF_BASE(".pll1",       CLK_PLL1, CLK_TYPE_GEN2_PLL1, CLK_MAIN),
	DEF_BASE(".pll3",       CLK_PLL3, CLK_TYPE_GEN2_PLL3, CLK_MAIN),

	DEF_FIXED(".pll1_div2", CLK_PLL1_DIV2, CLK_PLL1, 2, 1),

	/* Core Clock Outputs */
	DEF_BASE("z",    R8A7791_CLK_Z,    CLK_TYPE_GEN2_Z,    CLK_PLL0),
	DEF_BASE("lb",   R8A7791_CLK_LB,   CLK_TYPE_GEN2_LB,   CLK_PLL1),
	DEF_BASE("adsp", R8A7791_CLK_ADSP, CLK_TYPE_GEN2_ADSP, CLK_PLL1),
	DEF_BASE("sdh",  R8A7791_CLK_SDH,  CLK_TYPE_GEN2_SDH,  CLK_PLL1),
	DEF_BASE("sd0",  R8A7791_CLK_SD0,  CLK_TYPE_GEN2_SD0,  CLK_PLL1),
	DEF_BASE("qspi", R8A7791_CLK_QSPI, CLK_TYPE_GEN2_QSPI, CLK_PLL1_DIV2),
	DEF_BASE("rcan", R8A7791_CLK_RCAN, CLK_TYPE_GEN2_RCAN, CLK_USB_EXTAL),

	DEF_FIXED("zg",         R8A7791_CLK_ZG,    CLK_PLL1,          3, 1),
	DEF_FIXED("zx",         R8A7791_CLK_ZX,    CLK_PLL1,          3, 1),
	DEF_FIXED("zs",         R8A7791_CLK_ZS,    CLK_PLL1,          6, 1),
	DEF_FIXED("hp",         R8A7791_CLK_HP,    CLK_PLL1,         12, 1),
	DEF_FIXED("i",          R8A7791_CLK_I,     CLK_PLL1,          2, 1),
	DEF_FIXED("b",          R8A7791_CLK_B,     CLK_PLL1,         12, 1),
	DEF_FIXED("p",          R8A7791_CLK_P,     CLK_PLL1,         24, 1),
	DEF_FIXED("cl",         R8A7791_CLK_CL,    CLK_PLL1,         48, 1),
	DEF_FIXED("m2",         R8A7791_CLK_M2,    CLK_PLL1,          8, 1),
	DEF_FIXED("zb3",        R8A7791_CLK_ZB3,   CLK_PLL3,          4, 1),
	DEF_FIXED("zb3d2",      R8A7791_CLK_ZB3D2, CLK_PLL3,          8, 1),
	DEF_FIXED("ddr",        R8A7791_CLK_DDR,   CLK_PLL3,          8, 1),
	DEF_FIXED("mp",         R8A7791_CLK_MP,    CLK_PLL1_DIV2,    15, 1),
	DEF_FIXED("cp",         R8A7791_CLK_CP,    CLK_EXTAL,         2, 1),
	DEF_FIXED("r",          R8A7791_CLK_R,     CLK_PLL1,      49152, 1),
	DEF_FIXED("osc",        R8A7791_CLK_OSC,   CLK_PLL1,      12288, 1),

	DEF_DIV6P1("sd2",       R8A7791_CLK_SD2,   CLK_PLL1_DIV2, 0x078),
	DEF_DIV6P1("sd3",       R8A7791_CLK_SD3,   CLK_PLL1_DIV2, 0x26c),
	DEF_DIV6P1("mmc0",      R8A7791_CLK_MMC0,  CLK_PLL1_DIV2, 0x240),
	DEF_DIV6P1("ssp",       R8A7791_CLK_SSP,   CLK_PLL1_DIV2, 0x248),
	DEF_DIV6P1("ssprs",     R8A7791_CLK_SSPRS, CLK_PLL1_DIV2, 0x24c),
};

static const struct mssr_mod_clk r8a7791_mod_clks[] __initconst = {
	{ "msiof0",		0,	R8A7791_CLK_MP		},
	{ "vcp0",		101,	R8A7791_CLK_ZS		},
	{ "vpc0",		103,	R8A7791_CLK_ZS		},
	{ "jpu",		106,	R8A7791_CLK_M2		},
	{ "ssp1",		109,	R8A7791_CLK_ZS		},
	{ "tmu1",		111,	R8A7791_CLK_P		},
	{ "3dg",		112,	R8A7791_CLK_ZG		},
	{ "2ddmac",		115,	R8A7791_CLK_ZS		},
	{ "fdp1-1",		118,	R8A7791_CLK_ZS		},
	{ "fdp1-0",		119,	R8A7791_CLK_ZS		},
	{ "tmu3",		121,	R8A7791_CLK_P		},
	{ "tmu2",		122,	R8A7791_CLK_P		},
	{ "cmt0",		124,	R8A7791_CLK_R		},
	{ "tmu0",		125,	R8A7791_CLK_CP		},
	{ "vsp1-du1",		127,	R8A7791_CLK_ZS		},
	{ "vsp1-du0",		128,	R8A7791_CLK_ZS		},
	{ "vsp1-sy",		131,	R8A7791_CLK_ZS		},
	{ "scifa2",		202,	R8A7791_CLK_MP		},
	{ "scifa1",		203,	R8A7791_CLK_MP		},
	{ "scifa0",		204,	R8A7791_CLK_MP		},
	{ "msiof2",		205,	R8A7791_CLK_MP		},
	{ "scifb0",		206,	R8A7791_CLK_MP		},
	{ "scifb1",		207,	R8A7791_CLK_MP		},
	{ "msiof1",		208,	R8A7791_CLK_MP		},
	{ "scifb2",		216,	R8A7791_CLK_MP		},
	{ "sys-dmac1",		218,	R8A7791_CLK_ZS		},
	{ "sys-dmac0",		219,	R8A7791_CLK_ZS		},
	{ "tpu0",		304,	R8A7791_CLK_CP		},
	{ "sdhi2",		311,	R8A7791_CLK_SD3		},
	{ "sdhi1",		312,	R8A7791_CLK_SD2		},
	{ "sdhi0",		314,	R8A7791_CLK_SD0		},
	{ "mmcif0",		315,	R8A7791_CLK_MMC0	},
	{ "i2c7",		318,	R8A7791_CLK_HP		},
	{ "pciec",		319,	R8A7791_CLK_MP		},
	{ "i2c8",		323,	R8A7791_CLK_HP		},
	{ "ssusb",		328,	R8A7791_CLK_MP		},
	{ "cmt1",		329,	R8A7791_CLK_R		},
	{ "usbdmac0",		330,	R8A7791_CLK_HP		},
	{ "usbdmac1",		331,	R8A7791_CLK_HP		},
	{ "irqc",		407,	R8A7791_CLK_CP		},
	{ "intc-sys",		408,	R8A7791_CLK_ZS		},
	{ "audmac0",		502,	R8A7791_CLK_HP		},
	{ "audmac1",		501,	R8A7791_CLK_HP		},
	{ "adsp_mod",		506,	R8A7791_CLK_ADSP	},
	{ "thermal",		522,	CLK_EXTAL		},
	{ "pwm",		523,	R8A7791_CLK_P		},
	{ "ehci",		703,	R8A7791_CLK_MP		},
	{ "hsusb",		704,	R8A7791_CLK_HP		},
	{ "hscif2",		713,	R8A7791_CLK_ZS		},
	{ "scif5",		714,	R8A7791_CLK_P		},
	{ "scif4",		715,	R8A7791_CLK_P		},
	{ "hscif1",		716,	R8A7791_CLK_ZS		},
	{ "hscif0",		717,	R8A7791_CLK_ZS		},
	{ "scif3",		718,	R8A7791_CLK_P		},
	{ "scif2",		719,	R8A7791_CLK_P		},
	{ "scif1",		720,	R8A7791_CLK_P		},
	{ "scif0",		721,	R8A7791_CLK_P		},
	{ "du1",		723,	R8A7791_CLK_ZX		},
	{ "du0",		724,	R8A7791_CLK_ZX		},
	{ "lvds0",		726,	R8A7791_CLK_ZX		},
	{ "ipmmu_sgx",		800,	R8A7791_CLK_ZX		},
	{ "mlb",		802,	R8A7791_CLK_HP		},
	{ "vin2",		809,	R8A7791_CLK_ZG		},
	{ "vin1",		810,	R8A7791_CLK_ZG		},
	{ "vin0",		811,	R8A7791_CLK_ZG		},
	{ "ether",		813,	R8A7791_CLK_P		},
	{ "sata1",		814,	R8A7791_CLK_ZS		},
	{ "sata0",		815,	R8A7791_CLK_ZS		},
	{ "gpio7",		904,	R8A7791_CLK_CP		},
	{ "gpio6",		905,	R8A7791_CLK_CP		},
	{ "gpio5",		907,	R8A7791_CLK_CP		},
	{ "gpio4",		908,	R8A7791_CLK_CP		},
	{ "gpio3",		909,	R8A7791_CLK_CP		},
	{ "gpio2",		910,	R8A7791_CLK_CP		},
	{ "gpio1",		911,	R8A7791_CLK_CP		},
	{ "gpio0",		912,	R8A7791_CLK_CP		},
	{ "rcan1",		915,	R8A7791_CLK_P		},
	{ "rcan0",		916,	R8A7791_CLK_P		},
	{ "qspi_mod",		917,	R8A7791_CLK_QSPI	},
	{ "i2c5",		925,	R8A7791_CLK_HP		},
	{ "i2c6",		926,	R8A7791_CLK_CP		},
	{ "i2c4",		927,	R8A7791_CLK_HP		},
	{ "i2c3",		928,	R8A7791_CLK_HP		},
	{ "i2c2",		929,	R8A7791_CLK_HP		},
	{ "i2c1",		930,	R8A7791_CLK_HP		},
	{ "i2c0",		931,	R8A7791_CLK_HP		},
	{ "ssi-all",		1005,	R8A7791_CLK_P		},
	{ "ssi9",		1006,	R8A7791_CLK_P		},
	{ "ssi8",		1007,	R8A7791_CLK_P		},
	{ "ssi7",		1008,	R8A7791_CLK_P		},
	{ "ssi6",		1009,	R8A7791_CLK_P		},
	{ "ssi5",		1010,	R8A7791_CLK_P		},
	{ "ssi4",		1011,	R8A7791_CLK_P		},
	{ "ssi3",		1012,	R8A7791_CLK_P		},
	{ "ssi2",		1013,	R8A7791_CLK_P		},
	{ "ssi1",		1014,	R8A7791_CLK_P		},
	{ "ssi0",		1015,	R8A7791_CLK_P		},
	{ "scu-all",		1017,	R8A7791_CLK_P		},
	{ "scu-dvc1",		1018,	MOD_CLK_BASE + 1017	},
	{ "scu-dvc0",		1019,	MOD_CLK_BASE + 1017	},
	{ "scu-ctu1-mix1",	1020,	MOD_CLK_BASE + 1017	},
	{ "scu-ctu0-mix0",	1021,	MOD_CLK_BASE + 1017	},
	{ "scu-src9",		1022,	MOD_CLK_BASE + 1017	},
	{ "scu-src8",		1023,	MOD_CLK_BASE + 1017	},
	{ "scu-src7",		1024,	MOD_CLK_BASE + 1017	},
	{ "scu-src6",		1025,	MOD_CLK_BASE + 1017	},
	{ "scu-src5",		1026,	MOD_CLK_BASE + 1017	},
	{ "scu-src4",		1027,	MOD_CLK_BASE + 1017	},
	{ "scu-src3",		1028,	MOD_CLK_BASE + 1017	},
	{ "scu-src2",		1029,	MOD_CLK_BASE + 1017	},
	{ "scu-src1",		1030,	MOD_CLK_BASE + 1017	},
	{ "scu-src0",		1031,	MOD_CLK_BASE + 1017	},
	{ "scifa3",		1106,	R8A7791_CLK_MP		},
	{ "scifa4",		1107,	R8A7791_CLK_MP		},
	{ "scifa5",		1108,	R8A7791_CLK_MP		},
};

static const unsigned int r8a7791_crit_mod_clks[] __initconst = {
	408,	/* INTC-SYS (GIC) */
};


#define CPG_FRQCRB			0x00000004
#define CPG_FRQCRB_KICK			BIT(31)
#define CPG_SDCKCR			0x00000074
#define CPG_PLL0CR			0x000000d8
#define CPG_FRQCRC			0x000000e0
#define CPG_FRQCRC_ZFC_MASK		(0x1f << 8)
#define CPG_FRQCRC_ZFC_SHIFT		8
#define CPG_ADSPCKCR			0x0000025c
#define CPG_RCANCKCR			0x00000270

static spinlock_t cpg_lock;
static u32 cpg_mode __initdata;

/*
 * Z Clock
 *
 * Traits of this clock:
 * prepare - clk_prepare only ensures that parents are prepared
 * enable - clk_enable only ensures that parents are enabled
 * rate - rate is adjustable.  clk->rate = parent->rate * mult / 32
 * parent - fixed parent.  No clk_set_parent support
 */

struct cpg_z_clk {
	struct clk_hw hw;
	void __iomem *reg;
	void __iomem *kick_reg;
};

#define to_z_clk(_hw)	container_of(_hw, struct cpg_z_clk, hw)

static unsigned long cpg_z_clk_recalc_rate(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	struct cpg_z_clk *zclk = to_z_clk(hw);
	unsigned int mult;
	unsigned int val;

	val = (readl(zclk->reg) & CPG_FRQCRC_ZFC_MASK) >> CPG_FRQCRC_ZFC_SHIFT;
	mult = 32 - val;

	return div_u64((u64)parent_rate * mult, 32);
}

static long cpg_z_clk_round_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long *parent_rate)
{
	unsigned long prate  = *parent_rate;
	unsigned int mult;

	if (!prate)
		prate = 1;

	mult = div_u64((u64)rate * 32, prate);
	mult = clamp(mult, 1U, 32U);

	return *parent_rate / 32 * mult;
}

static int cpg_z_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long parent_rate)
{
	struct cpg_z_clk *zclk = to_z_clk(hw);
	unsigned int mult;
	u32 val, kick;
	unsigned int i;

	mult = div_u64((u64)rate * 32, parent_rate);
	mult = clamp(mult, 1U, 32U);

	if (readl(zclk->kick_reg) & CPG_FRQCRB_KICK)
		return -EBUSY;

	val = readl(zclk->reg);
	val &= ~CPG_FRQCRC_ZFC_MASK;
	val |= (32 - mult) << CPG_FRQCRC_ZFC_SHIFT;
	clk_writel(val, zclk->reg);

	/*
	 * Set KICK bit in FRQCRB to update hardware setting and wait for
	 * clock change completion.
	 */
	kick = readl(zclk->kick_reg);
	kick |= CPG_FRQCRB_KICK;
	clk_writel(kick, zclk->kick_reg);

	/*
	 * Note: There is no HW information about the worst case latency.
	 *
	 * Using experimental measurements, it seems that no more than
	 * ~10 iterations are needed, independently of the CPU rate.
	 * Since this value might be dependent on external xtal rate, pll1
	 * rate or even the other emulation clocks rate, use 1000 as a
	 * "super" safe value.
	 */
	for (i = 1000; i; i--) {
		if (!(readl(zclk->kick_reg) & CPG_FRQCRB_KICK))
			return 0;

		cpu_relax();
	}

	return -ETIMEDOUT;
}

static const struct clk_ops cpg_z_clk_ops = {
	.recalc_rate = cpg_z_clk_recalc_rate,
	.round_rate = cpg_z_clk_round_rate,
	.set_rate = cpg_z_clk_set_rate,
};

static struct clk * __init cpg_z_clk_register(const char *name,
					      const char *parent_name,
					      void __iomem *base)
{
	struct clk_init_data init;
	struct cpg_z_clk *zclk;
	struct clk *clk;

	zclk = kzalloc(sizeof(*zclk), GFP_KERNEL);
	if (!zclk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &cpg_z_clk_ops;
	init.flags = 0;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	zclk->reg = base + CPG_FRQCRC;
	zclk->kick_reg = base + CPG_FRQCRB;
	zclk->hw.init = &init;

	clk = clk_register(NULL, &zclk->hw);
	if (IS_ERR(clk))
		kfree(zclk);

	return clk;
}

static struct clk * __init cpg_rcan_clk_register(const char *name,
						 const char *parent_name,
						 void __iomem *base)
{
	struct clk_fixed_factor *fixed;
	struct clk_gate *gate;
	struct clk *clk;

	fixed = kzalloc(sizeof(*fixed), GFP_KERNEL);
	if (!fixed)
		return ERR_PTR(-ENOMEM);

	fixed->mult = 1;
	fixed->div = 6;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate) {
		kfree(fixed);
		return ERR_PTR(-ENOMEM);
	}

	gate->reg = base + CPG_RCANCKCR;
	gate->bit_idx = 8;
	gate->flags = CLK_GATE_SET_TO_DISABLE;
	gate->lock = &cpg_lock;

	clk = clk_register_composite(NULL, name, &parent_name, 1, NULL, NULL,
				     &fixed->hw, &clk_fixed_factor_ops,
				     &gate->hw, &clk_gate_ops, 0);
	if (IS_ERR(clk)) {
		kfree(gate);
		kfree(fixed);
	}

	return clk;
}

/* ADSP divisors */
static const struct clk_div_table cpg_adsp_div_table[] = {
	{  1,  3 }, {  2,  4 }, {  3,  6 }, {  4,  8 },
	{  5, 12 }, {  6, 16 }, {  7, 18 }, {  8, 24 },
	{ 10, 36 }, { 11, 48 }, {  0,  0 },
};

static struct clk * __init cpg_adsp_clk_register(const char *name,
						 const char *parent_name,
						 void __iomem *base)
{
	struct clk_divider *div;
	struct clk_gate *gate;
	struct clk *clk;

	div = kzalloc(sizeof(*div), GFP_KERNEL);
	if (!div)
		return ERR_PTR(-ENOMEM);

	div->reg = base + CPG_ADSPCKCR;
	div->width = 4;
	div->table = cpg_adsp_div_table;
	div->lock = &cpg_lock;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate) {
		kfree(div);
		return ERR_PTR(-ENOMEM);
	}

	gate->reg = base + CPG_ADSPCKCR;
	gate->bit_idx = 8;
	gate->flags = CLK_GATE_SET_TO_DISABLE;
	gate->lock = &cpg_lock;

	clk = clk_register_composite(NULL, name, &parent_name, 1, NULL, NULL,
				     &div->hw, &clk_divider_ops,
				     &gate->hw, &clk_gate_ops, 0);
	if (IS_ERR(clk)) {
		kfree(gate);
		kfree(div);
	}

	return clk;
}

/*
 * CPG Clock Data
 */

/*
 *   MD		EXTAL		PLL0	PLL1	PLL3
 * 14 13 19	(MHz)		*1	*1
 *---------------------------------------------------
 * 0  0  0	15 x 1		x172/2	x208/2	x106
 * 0  0  1	15 x 1		x172/2	x208/2	x88
 * 0  1  0	20 x 1		x130/2	x156/2	x80
 * 0  1  1	20 x 1		x130/2	x156/2	x66
 * 1  0  0	26 / 2		x200/2	x240/2	x122
 * 1  0  1	26 / 2		x200/2	x240/2	x102
 * 1  1  0	30 / 2		x172/2	x208/2	x106
 * 1  1  1	30 / 2		x172/2	x208/2	x88
 *
 * *1 :	Table 7.6 indicates VCO output (PLLx = VCO/2)
 */
#define CPG_PLL_CONFIG_INDEX(md)	((((md) & BIT(14)) >> 12) | \
					 (((md) & BIT(13)) >> 12) | \
					 (((md) & BIT(19)) >> 19))
struct cpg_pll_config {
	unsigned int extal_div;
	unsigned int pll1_mult;
	unsigned int pll3_mult;
};

static const struct cpg_pll_config cpg_pll_configs[8] __initconst = {
	{ 1, 208, 106 }, { 1, 208,  88 }, { 1, 156,  80 }, { 1, 156,  66 },
	{ 2, 240, 122 }, { 2, 240, 102 }, { 2, 208, 106 }, { 2, 208,  88 },
};

/* SDHI divisors */
static const struct clk_div_table cpg_sdh_div_table[] = {
	{  0,  2 }, {  1,  3 }, {  2,  4 }, {  3,  6 },
	{  4,  8 }, {  5, 12 }, {  6, 16 }, {  7, 18 },
	{  8, 24 }, { 10, 36 }, { 11, 48 }, {  0,  0 },
};

static const struct clk_div_table cpg_sd01_div_table[] = {
	{  4,  8 },
	{  5, 12 }, {  6, 16 }, {  7, 18 }, {  8, 24 },
	{ 10, 36 }, { 11, 48 }, { 12, 10 }, {  0,  0 },
};

static const struct cpg_pll_config *cpg_pll_config __initdata;

static
struct clk * __init r8a7791_cpg_clk_register(const struct cpg_core_clk *core,
					     const struct cpg_mssr_info *info,
					     struct clk **clks,
					     void __iomem *base)
{
	const struct clk_div_table *table = NULL;
	unsigned int idx = core->id;
	const struct clk *parent;
	const char *parent_name;
	unsigned int mult = 1;
	unsigned int div = 1;
	unsigned int shift;
	u32 value;

	pr_debug("Registering r8a7791 core clock %s id %u type %u\n",
		 core->name, idx, core->type);
	WARN_ON(idx >= info->num_total_core_clks);
	WARN_ON(PTR_ERR(clks[idx]) != -ENOENT);

	parent = clks[core->parent];
	if (IS_ERR(parent))
		return ERR_CAST(parent);

	parent_name = __clk_get_name(parent);

	switch (core->type) {
	/* R-Car Gen2 */
	case CLK_TYPE_GEN2_MAIN:
		div = cpg_pll_config->extal_div;
		break;

	case CLK_TYPE_GEN2_PLL0:
		/*
		 * PLL0 is a configurable multiplier clock. Register it as a
		 * fixed factor clock for now as there's no generic multiplier
		 * clock implementation and we currently have no need to change
		 * the multiplier value.
		 */
		value = readl(base + CPG_PLL0CR);
		mult = ((value >> 24) & ((1 << 7) - 1)) + 1;
		break;

	case CLK_TYPE_GEN2_PLL1:
		mult = cpg_pll_config->pll1_mult / 2;
		break;

	case CLK_TYPE_GEN2_PLL3:
		mult = cpg_pll_config->pll3_mult;
		break;

	case CLK_TYPE_GEN2_Z:
		return cpg_z_clk_register(core->name, parent_name, base);

	case CLK_TYPE_GEN2_LB:
		div = cpg_mode & BIT(18) ? 36 : 24;
		break;

	case CLK_TYPE_GEN2_ADSP:
		return cpg_adsp_clk_register(core->name, parent_name, base);

	case CLK_TYPE_GEN2_SDH:
		table = cpg_sdh_div_table;
		shift = 8;
		break;

	case CLK_TYPE_GEN2_SD0:
		table = cpg_sd01_div_table;
		shift = 4;
		break;

	case CLK_TYPE_GEN2_QSPI:
		div = (cpg_mode & (BIT(3) | BIT(2) | BIT(1))) == BIT(2) ?
		      8 : 10;
		break;

	case CLK_TYPE_GEN2_RCAN:
		return cpg_rcan_clk_register(core->name, parent_name, base);

	default:
		pr_err("%s: Unsupported clock type %u\n", __func__, core->type);
		return ERR_PTR(-EINVAL);
	}

	if (!table)
		return clk_register_fixed_factor(NULL, core->name, parent_name,
						 0, mult, div);
	else
		return clk_register_divider_table(NULL, core->name,
						  parent_name, 0,
						  base + CPG_SDCKCR, shift, 4,
						  0, table, &cpg_lock);
}

const struct cpg_mssr_info r8a7791_cpg_mssr_info __initconst = {
	/* Core Clocks */
	.core_clks = r8a7791_core_clks,
	.num_core_clks = ARRAY_SIZE(r8a7791_core_clks),
	.last_dt_core_clk = LAST_DT_CORE_CLK,
	.num_total_core_clks = MOD_CLK_BASE,

	/* Module Clocks */
	.mod_clks = r8a7791_mod_clks,
	.num_mod_clks = ARRAY_SIZE(r8a7791_mod_clks),
	.num_hw_mod_clks = 12 * 32,

	/* Critical Module Clocks */
	.crit_mod_clks = r8a7791_crit_mod_clks,
	.num_crit_mod_clks = ARRAY_SIZE(r8a7791_crit_mod_clks),

	/* Callbacks */
	.cpg_clk_register = r8a7791_cpg_clk_register,
};

static void __init r8a7791_cpg_mssr_init(struct device_node *np)
{
	struct regmap *regmap;
	u32 reg;

	regmap = syscon_regmap_lookup_by_phandle(np, "renesas,modemr");
	if (IS_ERR(regmap) ||
	    of_property_read_u32_index(np, "renesas,modemr", 1, &reg) ||
	    regmap_read(regmap, reg, &cpg_mode)) {
		/* Backward-compatibility with old DT */
		extern u32 rcar_gen2_read_mode_pins(void);

		pr_warn("%s: failed to parse renesas,modemr\n", np->full_name);
		cpg_mode = rcar_gen2_read_mode_pins();
	}

	cpg_pll_config = &cpg_pll_configs[CPG_PLL_CONFIG_INDEX(cpg_mode)];

	spin_lock_init(&cpg_lock);

	cpg_mssr_probe(np, &r8a7791_cpg_mssr_info);
}
CLK_OF_DECLARE(r8a7791_cpg_mssr, "renesas,r8a7791-cpg-mssr",
	       r8a7791_cpg_mssr_init);

// FIXME Allow building with a without clk-rcar-gen2
void __init __weak rcar_gen2_clocks_init(u32 mode)
{
	of_clk_init(NULL);
}
