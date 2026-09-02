// SPDX-License-Identifier: GPL-2.0
/*
 * R-Car X5H Clock Pulse Generator
 *
 * Copyright (C) 2026 Glider bv
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/dev_printk.h>
#include <linux/device-id/of.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <dt-bindings/clock/renesas,r8a78000-cpg.h>

struct clk_id_map_entry {
	u16 dt_id;		/* DT binding clock ID */
	u16 fw_id;		/* FIXED_CLK() ID */
};

struct clk_id_map {
	const struct clk_id_map_entry *entries;
	unsigned int size;
};

#define CLK_ID_MAP(_entries) &(struct clk_id_map) {	\
	.entries = _entries,				\
	.size = ARRAY_SIZE(_entries),			\
}

enum fixed_clk {
	FIXED_CLK_66M,
	FIXED_CLK_133M,
	FIXED_CLK_266M,
};

static const unsigned long fixed_clk_rates[] = {
	[FIXED_CLK_66M] = 66666000,
	[FIXED_CLK_133M] = 133330000,
	[FIXED_CLK_266M] = 266660000,
};

#define FIXED_CLK(rate)		FIXED_CLK_ ## rate

/**
 * struct r8a78000_cpg_priv - Clock Pulse Generator Private Data
 *
 * @dev: CPG device
 * @map: Mapping from DT clock IDs to fixed-rate clock IDs
 * @hws: Clocks, indexed by @map entry index
 */
struct r8a78000_cpg_priv {
	struct device *dev;
	const struct clk_id_map *map;
	struct clk_hw **hws;
};

static int clk_id_map_find(const struct clk_id_map *map, u32 dt_id)
{
	for (unsigned int i = 0; i < map->size; i++) {
		if (map->entries[i].dt_id == dt_id)
			return i;
	}

	return -1;
}

static struct clk_hw *r8a78000_clk_get(struct of_phandle_args *spec, void *data)
{
	struct r8a78000_cpg_priv *priv = data;
	struct device *dev = priv->dev;
	struct clk_hw *hw;
	int idx;
	u32 id;

	if (spec->args_count != 1)
		return ERR_PTR(-EINVAL);

	id = spec->args[0];

	idx = clk_id_map_find(priv->map, id);
	if (idx < 0) {
		dev_err(dev, "Unknown clock %u\n", id);
		return ERR_PTR(-ENOENT);
	}

	hw = priv->hws[idx];

	dev_dbg(dev, "Mapping DT clock %u to %s at %lu Hz\n", id,
		clk_hw_get_name(hw), clk_hw_get_rate(hw));

	return hw;
}

static struct clk_hw **register_fixed_clks(struct device *dev)
{
	unsigned int n = ARRAY_SIZE(fixed_clk_rates);
	unsigned long rate;
	struct clk_hw *hw;
	const char *name;

	struct clk_hw **hws __free(kfree) = kzalloc_objs(*hws, n);
	if (!hws)
		return ERR_PTR(-ENOMEM);

	for (unsigned int i = 0; i < n; i++) {
		rate = fixed_clk_rates[i];
		name = devm_kasprintf(dev, GFP_KERNEL, "cpg-%lu", rate);
		if (!name)
			return ERR_PTR(-ENOMEM);

		hw = devm_clk_hw_register_fixed_rate(dev, name, NULL, 0, rate);
		if (IS_ERR(hw))
			return ERR_CAST(hw);

		hws[i] = hw;
	}

	return_ptr(hws);
}

static struct clk_hw **get_hws(struct r8a78000_cpg_priv *priv)
{
	const struct clk_id_map *map = priv->map;
	struct device *dev = priv->dev;
	struct clk_hw **hws;
	u16 fw_id;

	struct clk_hw **fixed_hws __free(kfree) = register_fixed_clks(dev);
	if (IS_ERR(fixed_hws))
		return ERR_CAST(fixed_hws);

	hws = devm_kzalloc_objs(dev, *hws, map->size);
	if (!hws)
		return ERR_PTR(-ENOMEM);

	for (unsigned int i = 0; i < map->size; i++) {
		fw_id = map->entries[i].fw_id;
		hws[i] = fixed_hws[fw_id];
	}

	return hws;
}

static int r8a78000_cpg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct r8a78000_cpg_priv *priv;
	const struct clk_id_map *map;

	map = of_device_get_match_data(dev);
	if (!map)
		return -ENODEV;

	priv = devm_kzalloc_obj(dev, *priv);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->map = map;

	priv->hws = get_hws(priv);
	if (IS_ERR(priv->hws))
		return PTR_ERR(priv->hws);

	return devm_of_clk_add_hw_provider(dev, r8a78000_clk_get, priv);
}

static const struct clk_id_map_entry r8a78000_cpg_default[] = {
	{ R8A78000_CPG_SGASYNCD4_PERW_BUS,	FIXED_CLK(266M) },
	{ R8A78000_CPG_SGASYNCD8_PERW_BUS,	FIXED_CLK(133M) },
	{ R8A78000_CPG_SGASYNCD16_PERW_BUS,	FIXED_CLK(66M) },
};

static const struct of_device_id r8a78000_cpg_match[] = {
	{
		.compatible = "renesas,r8a78000-cpg",
		.data = CLK_ID_MAP(r8a78000_cpg_default),
	},
	{ /* sentinel */ }
};

static struct platform_driver r8a78000_cpg_driver = {
	.probe = r8a78000_cpg_probe,
	.driver = {
		.name = "r8a78000-cpg",
		.of_match_table = r8a78000_cpg_match,
		.suppress_bind_attrs = true,
	},
};

builtin_platform_driver(r8a78000_cpg_driver)

MODULE_DESCRIPTION("R-Car X5H CPG Driver");
