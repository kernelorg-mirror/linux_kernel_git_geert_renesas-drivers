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
#include <linux/scmi_protocol.h>
#include <linux/slab.h>

#include <dt-bindings/clock/renesas,r8a78000-cpg.h>

struct clk_id_map_entry {
	u16 dt_id;		/* DT binding clock ID */
	u16 fw_id;		/* SCMI firmware clock ID or FIXED_CLK() ID */
};

struct clk_id_map {
	const struct clk_id_map_entry *entries;
	unsigned int size;
};

#define CLK_ID_MAP(_entries) &(struct clk_id_map) {	\
	.entries = _entries,				\
	.size = ARRAY_SIZE(_entries),			\
}

struct cpg_data {
	const struct clk_id_map *default_map;
	const struct clk_id_map *fw_map;
};

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

#define FIXED_CLK_OFFSET	0x8000
#define FIXED_CLK(rate)		(FIXED_CLK_OFFSET + FIXED_CLK_ ## rate)

/**
 * struct r8a78000_cpg_priv - Clock Pulse Generator Private Data
 *
 * @dev: CPG device
 * @map: Mapping from DT clock IDs to SCMI or fixed-rate clock IDs
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
	if (!hw) {
		/* CLOCK_ATTRIBUTES is not supported */
		dev_err(dev, "Clock %u is not available\n", id);
		return ERR_PTR(-ENOENT);
	}

	dev_dbg(dev, "Mapping DT clock %u to %s at %lu Hz\n", id,
		clk_hw_get_name(hw), clk_hw_get_rate(hw));

	return hw;
}

static struct device_node *scmi_find_proto(struct device_node *scmi, u32 proto)
{
	for_each_available_child_of_node_scoped(scmi, child) {
		u32 reg;

		if (of_property_read_u32(child, "reg", &reg))
			continue;

		if (reg == proto)
			return_ptr(child);
	}

	return NULL;
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

static struct clk_hw *get_fallback(const struct clk_id_map *map, u16 dt_id,
				   struct clk_hw **fixed_hws)
{
	for (unsigned int i = 0; i < map->size; i++) {
		if (map->entries[i].dt_id == dt_id)
			return fixed_hws[map->entries[i].fw_id - FIXED_CLK_OFFSET];
	}

	return NULL;
}

static struct clk_hw **get_hws(struct r8a78000_cpg_priv *priv,
			       const struct clk_id_map *fallback,
			       struct device_node *scmi_clk_np)
{
	const struct clk_id_map *map = priv->map;
	struct of_phandle_args scmi_spec;
	struct device *dev = priv->dev;
	struct clk_hw **hws, *hw;
	struct clk *clk;
	u16 fw_id;

	struct clk_hw **fixed_hws __free(kfree) = register_fixed_clks(dev);
	if (IS_ERR(fixed_hws))
		return ERR_CAST(fixed_hws);

	hws = devm_kzalloc_objs(dev, *hws, map->size);
	if (!hws)
		return ERR_PTR(-ENOMEM);

	for (unsigned int i = 0; i < map->size; i++) {
		fw_id = map->entries[i].fw_id;
		if (fw_id >= FIXED_CLK_OFFSET) {
			hws[i] = fixed_hws[fw_id - FIXED_CLK_OFFSET];
			continue;
		}

		scmi_spec.np = scmi_clk_np;
		scmi_spec.args_count = 1;
		scmi_spec.args[0] = fw_id;

		clk = of_clk_get_from_provider(&scmi_spec);
		if (IS_ERR(clk))
			return dev_err_cast_probe(dev, clk,
				"Failed to get SCMI clock %u\n", fw_id);

		hw = __clk_get_hw(clk);
		clk_put(clk);
		if (!hw)
			hw = get_fallback(fallback, map->entries[i].dt_id,
					  fixed_hws);
		if (!hw) {
			/* CLOCK_ATTRIBUTES is not supported */
			dev_warn(dev, "SCMI clock %u is NULL\n", fw_id);
			continue;
		}

		dev_dbg(priv->dev, "SCMI clock %u is %s at %lu Hz\n", fw_id,
			clk_hw_get_name(hw), clk_hw_get_rate(hw));

		hws[i] = hw;
	}

	return hws;
}

static int r8a78000_cpg_probe(struct platform_device *pdev)
{
	struct device_node *scmi_clk_np __free(device_node) = NULL;
	struct device_node *scmi __free(device_node) = NULL;
	struct device *dev = &pdev->dev;
	const struct cpg_data *cpg_data;
	struct r8a78000_cpg_priv *priv;
	const struct clk_id_map *map;

	cpg_data = of_device_get_match_data(dev);
	if (!cpg_data)
		return -ENODEV;

	map = cpg_data->default_map;

	priv = devm_kzalloc_obj(dev, *priv);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	if (!IS_ENABLED(CONFIG_COMMON_CLK_SCMI)) {
		dev_dbg(dev, "SCMI clock support is disabled\n");
		goto fallback;
	}

	scmi = of_find_compatible_node(NULL, NULL, "arm,scmi");
	if (!scmi) {
		dev_dbg(dev, "Cannot find SCMI firmware node\n");
		goto fallback;
	}

	if (!of_device_is_available(scmi)) {
		dev_dbg(dev, "SCMI firmware node is not available\n");
		goto fallback;
	}

	scmi_clk_np = scmi_find_proto(scmi, SCMI_PROTOCOL_CLOCK);
	if (!scmi_clk_np) {
		dev_dbg(dev, "Cannot find SCMI clock management protocol\n");
		goto fallback;
	}

	map = cpg_data->fw_map;

fallback:
	priv->map = map;

	/*
	 * We cannot do lazy look-up in r8a78000_clk_get(), as that function is
	 * called with of_clk_mutex already held.
	 */
	priv->hws = get_hws(priv, cpg_data->default_map, scmi_clk_np);
	if (IS_ERR(priv->hws))
		return PTR_ERR(priv->hws);

	return devm_of_clk_add_hw_provider(dev, r8a78000_clk_get, priv);
}

static const struct clk_id_map_entry r8a78000_cpg_default[] = {
	{ R8A78000_CPG_SGASYNCD4_PERW_BUS,	FIXED_CLK(266M) },
	{ R8A78000_CPG_SGASYNCD8_PERW_BUS,	FIXED_CLK(133M) },
	{ R8A78000_CPG_SGASYNCD16_PERW_BUS,	FIXED_CLK(66M) },
};

static const struct clk_id_map_entry r8a78000_cpg_fw[] = {
	{ R8A78000_CPG_SGASYNCD4_PERW_BUS,	1661 },
	{ R8A78000_CPG_SGASYNCD8_PERW_BUS,	1662 },
	{ R8A78000_CPG_SGASYNCD16_PERW_BUS,	1663 },
	{ R8A78000_CPG_MSO_SCP_MAIN,		1459 },
	{ R8A78000_CPG_MSOCK_PERW_BUS,		1667 },
};

static const struct cpg_data r8a78000_cpg_data = {
	.default_map = CLK_ID_MAP(r8a78000_cpg_default),
	.fw_map = CLK_ID_MAP(r8a78000_cpg_fw),
};

static const struct of_device_id r8a78000_cpg_match[] = {
	{
		.compatible = "renesas,r8a78000-cpg",
		.data = &r8a78000_cpg_data,
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
