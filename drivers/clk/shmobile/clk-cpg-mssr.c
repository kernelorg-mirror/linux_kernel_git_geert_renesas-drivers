/*
 * Renesas Clock Pulse Generator / Module Standby and Software Reset
 *
 * Copyright (C) 2015 Glider bvba
 *
 * Based on clk-mstp.c, clk-rcar-gen2.c, and clk-rcar-gen3.c
 *
 * Copyright (C) 2013 Ideas On Board SPRL
 * Copyright (C) 2015 Renesas Electronics Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_clock.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>

#include <dt-bindings/clock/renesas-cpg-mssr.h>

#include "clk-cpg-mssr.h"
#include "clk-div6.h"


/*
 * Module Standby and Software Reset register offets.
 *
 * If the registers exist, these are valid for SH-Mobile, R-Mobile,
 * R-Car Gen 2, and R-Car Gen 3.
 * These are NOT valid for R-Car Gen1 and RZ/A1!
 */

/*
 * Module Stop Status Register offsets
 */

static const u16 mstpsr[] = {
	0x030, 0x038, 0x040, 0x048, 0x04C, 0x03C, 0x1C0, 0x1C4,
	0x9A0, 0x9A4, 0x9A8, 0x9AC,
};

#define	MSTPSR(i)	mstpsr[i]


/*
 * System Module Stop Control Register offsets
 */

static const u16 smstpcr[] = {
	0x130, 0x134, 0x138, 0x13C, 0x140, 0x144, 0x148, 0x14C,
	0x990, 0x994, 0x998, 0x99C,
};

#define	SMSTPCR(i)	smstpcr[i]


/*
 * Software Reset Register offsets
 */

static const u16 srcr[] = {
	0x0A0, 0x0A8, 0x0B0, 0x0B8, 0x0BC, 0x0C4, 0x1C8, 0x1CC,
	0x920, 0x924, 0x928, 0x92C,
};

#define	SRCR(i)		srcr[i]


/* Realtime Module Stop Control Register offsets */
#define RMSTPCR(i)	(smstpcr[i] - 0x20)

/* Modem Module Stop Control Register offsets (r8a73a4) */
#define MMSTPCR(i)	(smstpcr[i] + 0x20)

/* Software Reset Clearing Register offsets */
#define	SRSTCLR(i)	(0x940 + (i) * 4)


#define MSTP_MAX_REGS		ARRAY_SIZE(smstpcr)
#define MSTP_MAX_CLOCKS		(MSTP_MAX_REGS * 32)


/**
 * Clock Pulse Generator / Module Standby and Software Reset Private Data
 *
 * @base: CPG/MSSR register block base address
 * @mstp_lock: protects writes to SMSTPCR
 */
struct cpg_mssr_priv {
	void __iomem *base;
	spinlock_t mstp_lock;

	/* Core and Module Clocks */
	struct clk **clks;

	/* Core Clocks */
	unsigned int num_core_clks;
	unsigned int last_dt_core_clk;

	/* Module Clocks */
	unsigned int num_mod_clks;

	/* Core Clocks suitable for PM, in addition to the module clocks */
	const unsigned int *core_pm_clks;
	unsigned int num_core_pm_clks;
};


/**
 * struct mstp_clock - MSTP gating clock
 * @hw: handle between common and hardware-specific interfaces
 * @index: MSTP clock number
 * @priv: CPG/MSSR private data
 */
struct mstp_clock {
	struct clk_hw hw;
	u32 index;
	struct cpg_mssr_priv *priv;
};

#define to_mstp_clock(_hw) container_of(_hw, struct mstp_clock, hw)

static int cpg_mstp_clock_endisable(struct clk_hw *hw, bool enable)
{
	struct mstp_clock *clock = to_mstp_clock(hw);
	struct cpg_mssr_priv *priv = clock->priv;
	unsigned int reg = clock->index / 32;
	unsigned int bit = clock->index % 32;
	u32 bitmask = BIT(bit);
	unsigned long flags;
	unsigned int i;
	u32 value;

	pr_debug("MSTP %u%02u/%pC %s\n", reg, bit, hw->clk,
		 enable ? "ON" : "OFF");
	spin_lock_irqsave(&priv->mstp_lock, flags);

	value = clk_readl(priv->base + SMSTPCR(reg));
	if (enable)
		value &= ~bitmask;
	else
		value |= bitmask;
	clk_writel(value, priv->base + SMSTPCR(reg));

	spin_unlock_irqrestore(&priv->mstp_lock, flags);

	if (!enable)
		return 0;

	for (i = 1000; i > 0; --i) {
		if (!(clk_readl(priv->base + MSTPSR(reg)) &
		      bitmask))
			break;
		cpu_relax();
	}

	if (!i) {
		pr_err("%s: Failed to enable %p[%d]\n", __func__,
		       priv->base + SMSTPCR(reg), bit);
		return -ETIMEDOUT;
	}

	return 0;
}

static int cpg_mstp_clock_enable(struct clk_hw *hw)
{
	return cpg_mstp_clock_endisable(hw, true);
}

static void cpg_mstp_clock_disable(struct clk_hw *hw)
{
	cpg_mstp_clock_endisable(hw, false);
}

static int cpg_mstp_clock_is_enabled(struct clk_hw *hw)
{
	struct mstp_clock *clock = to_mstp_clock(hw);
	struct cpg_mssr_priv *priv = clock->priv;
	u32 value;

	value = clk_readl(priv->base + MSTPSR(clock->index / 32));

	return !(value & BIT(clock->index % 32));
}

static const struct clk_ops cpg_mstp_clock_ops = {
	.enable = cpg_mstp_clock_enable,
	.disable = cpg_mstp_clock_disable,
	.is_enabled = cpg_mstp_clock_is_enabled,
};

static
struct clk *cpg_mssr_clk_src_twocell_get(struct of_phandle_args *clkspec,
					 void *data)
{
	unsigned int clkidx = clkspec->args[1];
	struct cpg_mssr_priv *priv = data;
	unsigned int idx;
	struct clk *clk;

	switch (clkspec->args[0]) {
	case CPG_CORE:
		if (clkidx > priv->last_dt_core_clk) {
			pr_err("%s: Invalid %s clock index %u\n", __func__,
			       "core", clkidx);
			return ERR_PTR(-EINVAL);
		}
		clk = priv->clks[clkidx];
		break;

	case CPG_MOD:
		/* Translate from sparse base-100 to packed index space */
		idx = clkidx - (clkidx / 100) * (100 - 32);
		if (clkidx % 100 > 31 || idx >= priv->num_mod_clks) {
			pr_err("%s: Invalid %s clock index %u\n", __func__,
			       "module", clkidx);
			return ERR_PTR(-EINVAL);
		}
		clk = priv->clks[priv->num_core_clks + idx];
		break;

	default:
		pr_err("%s: Invalid CPG clock type %u\n", __func__,
		       clkspec->args[0]);
		return ERR_PTR(-EINVAL);
	}

	if (IS_ERR(clk))
		pr_err("%s: Cannot get %s clock %u: %ld", __func__,
		       clkspec->args[0] == CPG_CORE ? "core" : "module",
		       clkidx, PTR_ERR(clk));
	else
		pr_debug("%s: clock (%u, %u) is %pC\n", __func__,
			 clkspec->args[0], clkspec->args[1], clk);
	return clk;
}


// FIXME Handle packing in a macro in cpg_mssr_info.mod_clks[]???
static unsigned int __init mod_pack(unsigned int idx,
				    struct cpg_mssr_priv *priv)
{
	WARN_ON(idx % 100 > 31);
	idx -= (idx / 100) * (100 - 32);
	WARN_ON(idx >= priv->num_mod_clks);
	return idx;
}

static unsigned int __init id_to_idx(unsigned int id,
				     struct cpg_mssr_priv *priv)
{
	unsigned int idx;

	if (id < priv->num_core_clks) {
		/* Core Clock */
		pr_debug("%s: %u is a core clock\n", __func__, id);
		return id;
	}

	/* Module Clock */
	idx = priv->num_core_clks +
	      mod_pack(id - priv->num_core_clks, priv);
	pr_debug("%s: %u is module clock %u at index %u\n", __func__,
		 id, id - priv->num_core_clks, idx);
	return idx;
}

static void __init cpg_mssr_register_core_clk(struct device_node *np,
					      const struct cpg_core_clk *core,
					      const struct cpg_mssr_info *info,
					      struct cpg_mssr_priv *priv)
{
	struct clk *clk = NULL, *parent;
	unsigned int idx = core->id;
	const char *parent_name;

	pr_debug("Registering core clock %s id %u type %u\n", core->name, idx,
		 core->type);
	WARN_ON(idx >= priv->num_core_clks);
	WARN_ON(PTR_ERR(priv->clks[idx]) != -ENOENT);

	switch (core->type) {
	/* Generic */
	case CLK_TYPE_IN:
		/* External Clock Input */
		clk = of_clk_get_by_name(np, core->name);
		break;

	case CLK_TYPE_FF:
		/* Fixed Factor Clock */
		WARN_ON(core->parent >= priv->num_core_clks);
		parent = priv->clks[core->parent];
		if (IS_ERR(parent)) {
			clk = parent;
			goto fail;
		}

		parent_name = __clk_get_name(parent);
		clk = clk_register_fixed_factor(NULL, core->name, parent_name,
						0, core->mult, core->div);
		break;

	case CLK_TYPE_DIV6P1:
		/* DIV6 Clock with 1 parent clock */
		WARN_ON(core->parent >= priv->num_core_clks);
		parent = priv->clks[core->parent];
		if (IS_ERR(parent)) {
			clk = parent;
			goto fail;
		}

		parent_name = __clk_get_name(parent);
		clk = cpg_div6_register(core->name, 1, &parent_name,
					priv->base + core->offset);
		break;

	default:
		if (info->cpg_clk_register)
			clk = info->cpg_clk_register(core, info, priv->clks,
						     priv->base);
		else
			pr_err("%s: Unsupported clock type %u\n", __func__,
			       core->type);
		break;
	}

	if (IS_ERR_OR_NULL(clk))
		goto fail;

	pr_debug("%s: Registered core clock %pC\n", __func__, clk);
	priv->clks[idx] = clk;
	return;

fail:
	pr_err("%s: Failed to register core clock %u: %ld\n", __func__, idx,
	       PTR_ERR(clk));
}

static void __init cpg_mssr_register_mod_clk(const struct mssr_mod_clk *mod,
					     const struct cpg_mssr_info *info,
					     struct cpg_mssr_priv *priv)
{
	unsigned int mod_idx, idx, parent_idx;
	struct mstp_clock *clock = NULL;
	struct clk_init_data init;
	struct clk *parent, *clk;
	const char *parent_name;
	unsigned int i;

	pr_debug("Registering module clock %s id %u parent %u\n", mod->name,
		 mod->id, mod->parent);

	// FIXME Incorporate offset/packing in cpg_mssr_info.mod_clks[]?
	mod_idx = mod_pack(mod->id, priv);
	WARN_ON(mod_idx >= priv->num_mod_clks);
	idx = priv->num_core_clks + mod_idx;
	parent_idx = id_to_idx(mod->parent, priv);
	WARN_ON(parent_idx >= priv->num_core_clks + priv->num_mod_clks);
	WARN_ON(PTR_ERR(priv->clks[idx]) != -ENOENT);

	parent = priv->clks[parent_idx];
	if (IS_ERR(parent)) {
		clk = parent;
		goto fail;
	}

	clock = kzalloc(sizeof(*clock), GFP_KERNEL);
	if (!clock) {
		clk = ERR_PTR(-ENOMEM);
		goto fail;
	}

	init.name = mod->name;
	init.ops = &cpg_mstp_clock_ops;
	init.flags = CLK_IS_BASIC | CLK_SET_RATE_PARENT;
	for (i = 0; i < info->num_crit_mod_clks; i++)
		if (mod->id == info->crit_mod_clks[i]) {
#ifdef CLK_ENABLE_HAND_OFF // FIXME Not yet in -next
			pr_debug("%s: MSTP %s setting CLK_ENABLE_HAND_OFF\n",
				 __func__, mod->name);
			init.flags |= CLK_ENABLE_HAND_OFF;
			break;
#else
			pr_debug("%s: Ignoring MSTP %s to prevent disabling\n",
				 __func__, mod->name);
			return;
#endif
		}

	parent_name = __clk_get_name(parent);
	init.parent_names = &parent_name;
	init.num_parents = 1;

	clock->index = mod_idx;
	clock->priv = priv;
	clock->hw.init = &init;

	clk = clk_register(NULL, &clock->hw);
	if (IS_ERR(clk))
		goto fail;

	pr_debug("%s: Created module clock %pC\n", __func__, clk);
	priv->clks[idx] = clk;
	return;

fail:
	pr_err("%s: Failed to create module clock %u: %ld\n", __func__,
	       mod->id, PTR_ERR(clk));
	kfree(clock);
}


#ifdef CONFIG_PM_GENERIC_DOMAINS_OF
static const struct of_device_id cpg_mssr_match[] = {
	// FIXME Reuse the string as matched by the driver
	{ .compatible = "renesas,r8a7791-cpg-mssr", },
	{ .compatible = "renesas,r8a7795-cpg-mssr", },
	{ /* sentinel */ }
};

static bool cpg_mssr_is_pm_clk(const struct of_phandle_args *clkspec)
{
	if (!of_match_node(cpg_mssr_match, clkspec->np))
		return false;

	if (clkspec->args_count != 2)
		return false;

	switch (clkspec->args[0]) {
	case CPG_CORE:
		// FIXME Match against cpg_mssr_info.core_pm_clks[]
		return false;

	case CPG_MOD:
		return true;

	default:
		return false;
	}
}

static int cpg_mssr_attach_dev(struct generic_pm_domain *domain,
			       struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct of_phandle_args clkspec;
	struct clk *clk;
	int i = 0;
	int error;

	while (!of_parse_phandle_with_args(np, "clocks", "#clock-cells", i,
					   &clkspec)) {
		if (cpg_mssr_is_pm_clk(&clkspec))
			goto found;

		of_node_put(clkspec.np);
		i++;
	}

	return 0;

found:
	clk = of_clk_get_from_provider(&clkspec);
	of_node_put(clkspec.np);

	if (IS_ERR(clk))
		return PTR_ERR(clk);

	error = pm_clk_create(dev);
	if (error) {
		dev_err(dev, "pm_clk_create failed %d\n", error);
		goto fail_put;
	}

	error = pm_clk_add_clk(dev, clk);
	if (error) {
		dev_err(dev, "pm_clk_add_clk %pC failed %d\n", clk, error);
		goto fail_destroy;
	}

	return 0;

fail_destroy:
	pm_clk_destroy(dev);
fail_put:
	clk_put(clk);
	return error;
}

static void cpg_mssr_detach_dev(struct generic_pm_domain *domain,
				struct device *dev)
{
	if (!list_empty(&dev->power.subsys_data->clock_list))
		pm_clk_destroy(dev);
}

static void __init cpg_mssr_add_clk_domain(struct device_node *np)
{
	struct generic_pm_domain *pd;
	u32 ncells;

	if (of_property_read_u32(np, "#power-domain-cells", &ncells)) {
		pr_warn("%s lacks #power-domain-cells\n", np->full_name);
		return;
	}

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return;

	pd->name = np->name;

	pd->flags = GENPD_FLAG_PM_CLK;
	pm_genpd_init(pd, &simple_qos_governor, false);
	pd->attach_dev = cpg_mssr_attach_dev;
	pd->detach_dev = cpg_mssr_detach_dev;

	of_genpd_add_provider_simple(np, pd);
}
#else
static inline void cpg_mssr_add_clk_domain(struct device_node *np) {}
#endif /* !CONFIG_PM_GENERIC_DOMAINS_OF */


void __init cpg_mssr_probe(struct device_node *np,
			   const struct cpg_mssr_info *info)
{
	struct cpg_mssr_priv *priv;
	unsigned int nclks, i;
	struct clk **clks;

	pr_debug("%s: Using %ps\n", __func__, info);
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return;

	priv->base = of_iomap(np, 0);
	if (!priv->base)
		return;

	nclks = info->num_total_core_clks + info->num_hw_mod_clks;
	clks = kmalloc_array(nclks, sizeof(*clks), GFP_KERNEL);
	if (!clks)
		return;

	spin_lock_init(&priv->mstp_lock);
	priv->clks = clks;
	priv->num_core_clks = info->num_total_core_clks;
	priv->last_dt_core_clk = info->last_dt_core_clk;
	priv->num_mod_clks = info->num_hw_mod_clks;

	for (i = 0; i < nclks; i++)
		clks[i] = ERR_PTR(-ENOENT);

	for (i = 0; i < info->num_core_clks; i++)
		cpg_mssr_register_core_clk(np, &info->core_clks[i], info,
					   priv);

	for (i = 0; i < info->num_mod_clks; i++)
		cpg_mssr_register_mod_clk(&info->mod_clks[i], info, priv);

	of_clk_add_provider(np, cpg_mssr_clk_src_twocell_get, priv);

	cpg_mssr_add_clk_domain(np);
}

MODULE_DESCRIPTION("Renesas CPG/MSSR Driver");
MODULE_LICENSE("GPL v2");
