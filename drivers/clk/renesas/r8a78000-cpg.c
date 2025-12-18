// SPDX-License-Identifier: GPL-2.0
/*
 * r8a78000 Clock Pulse Generator
 *
 * Copyright (C) 2026 Glider bv
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_clock.h>
#include <linux/pm_domain.h>
#include <linux/scmi_protocol.h>
#include <linux/slab.h>

#include <dt-bindings/clock/renesas,r8a78000-cpg.h>

struct clk_map {
	int dt_id;		/* DT binding clock ID */
	u32 scmi_id;		/* SCMI clock ID */
	struct clk *clk;
};

struct r8a78000_cpg_info {
	/* FIXME const */ struct clk_map *map;
};

enum fixed_clk {
	FIXED_CLK_66M,
	FIXED_CLK_266M,
	NUM_FIXED_CLKS
};

static const unsigned long fixed_clk_rates[NUM_FIXED_CLKS] = {
	[FIXED_CLK_66M] = 66666000,
	[FIXED_CLK_266M] = 266660000,
};

#define FIXED_CLK_OFFSET	0x80000000
#define FIXED_CLK(rate)		FIXED_CLK_OFFSET + FIXED_CLK_ ## rate

/**
 * struct r8a78000_cpg_priv - Clock Pulse Generator Private Data
 *
 * @dev: CPG device
 * @scmi_clk_np: Device node in DT for the SCMI firmware clock protocol
 * @map: Mapping from DT clock IDs to SCMI clocks
 * @fixed_clks: Fixed rate clocks used to replace SCMI clocks that do not
 *              support the SCMI CLOCK_ATTRIBUTES command
 */
struct r8a78000_cpg_priv {
	struct device *dev;
	struct device_node *scmi_clk_np;
	const struct clk_map *map;
	struct clk *fixed_clks[NUM_FIXED_CLKS];
};

static const struct clk_map *clk_map_find(const struct clk_map *map, u32 id)
{
	if (!map)
		return NULL;

	for (; map->dt_id >= 0; map++) {
		if (map->dt_id == id)
			return map;
	}

	return NULL;
}

static struct clk_hw *r8a78000_clk_get(struct of_phandle_args *spec,
				      void *data)
{
	struct r8a78000_cpg_priv *priv = data;
	struct device *dev = priv->dev;
	const struct clk_map *map;
	struct clk_hw *hw;
	struct clk *clk;
	u32 id;

	if (spec->args_count != 1)
		return ERR_PTR(-EINVAL);

	id = spec->args[0];

	map = clk_map_find(priv->map, id);
	if (!map) {
		dev_err(dev, "Unknown clock %u\n", id);
		return ERR_PTR(-ENOENT);
	}

	if (map->scmi_id < FIXED_CLK_OFFSET) {
dev_info(dev, "Mapping clock %u to SCMI clock %u\n", id, map->scmi_id);

		clk = map->clk;
	} else {
		u32 idx = map->scmi_id - FIXED_CLK_OFFSET;

dev_info(dev, "Mapping clock %u to fixed clock %u\n", id, idx);
		clk = priv->fixed_clks[idx];
	}

	hw = __clk_get_hw(clk);
	if (IS_ERR(hw)) {
		dev_err(dev, "Cannot get clock %u: %pe\n", id, hw);
		return hw;
	}

	if (!hw) {
		// FIXME NULL if CLOCK_ATTRIBUTES not supported
		dev_err(dev, "Clock %u is NULL!\n", id);
		return ERR_PTR(-ENOENT);
	}

	// FIXME dev_dbg
	dev_info(dev, "clock %u is %s at %lu Hz\n", id, clk_hw_get_name(hw),
		 clk_hw_get_rate(hw));

	return hw;
}

static struct device_node *scmi_find_clk_np(struct device *dev)
{
	struct device_node *scmi __free(device_node);

	scmi = of_find_compatible_node(NULL, NULL, "arm,scmi");
	if (!scmi) {
		dev_err(dev, "Cannot find SCMI firmware node\n");
		return NULL;
	}

	for_each_available_child_of_node_scoped(scmi, child) {
		u32 proto;

		if (of_property_read_u32(child, "reg", &proto))
			continue;

		if (proto == SCMI_PROTOCOL_CLOCK)
			return_ptr(child);
	}

	return NULL;
}

static int prefill_scmi_clks(struct r8a78000_cpg_priv *priv,
			     struct clk_map *map)
{
	struct of_phandle_args scmi_spec;

	for (; map->dt_id >= 0; map++) {
		if (map->scmi_id >= FIXED_CLK_OFFSET)
			continue;

		scmi_spec.np = priv->scmi_clk_np;
		scmi_spec.args_count = 1;
		scmi_spec.args[0] = map->scmi_id;

		map->clk = of_clk_get_from_provider(&scmi_spec);
		if (IS_ERR(map->clk))
			return dev_err_probe(priv->dev, PTR_ERR(map->clk),
					     "Failed to get SCMI clock %u\n",
					     map->scmi_id);

dev_info(priv->dev, "SCMI clock %u is %pC\n", map->scmi_id, map->clk);
	}

	return 0;
}

static void unregister_fixed_clks(void *data)
{
	struct r8a78000_cpg_priv *priv = data;

	for (unsigned int i = 0; i < ARRAY_SIZE(priv->fixed_clks); i++)
		clk_unregister_fixed_rate(priv->fixed_clks[i]);
}

static int register_fixed_clks(struct r8a78000_cpg_priv *priv)
{
	struct device *dev = priv->dev;
	unsigned long rate;
	const char *name;
	struct clk *clk;

	for (unsigned int i = 0; i < ARRAY_SIZE(fixed_clk_rates); i++) {
		rate = fixed_clk_rates[i];
		name = devm_kasprintf(dev, GFP_KERNEL, "cpg-%lu", rate);
		if (!name)
			return -ENOMEM;

		clk = clk_register_fixed_rate(dev, name, NULL, 0, rate);
		if (IS_ERR(clk)) {
			while (i-- > 0)
				clk_unregister_fixed_rate(priv->fixed_clks[i]);
			return PTR_ERR(clk);
		}

		priv->fixed_clks[i] = clk;
	}

	return devm_add_action_or_reset(dev, unregister_fixed_clks, priv);
}

static int /* __init */ r8a78000_cpg_probe(struct platform_device *pdev)
{
	const struct r8a78000_cpg_info *info;
	struct device *dev = &pdev->dev;
	struct r8a78000_cpg_priv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	priv->scmi_clk_np = scmi_find_clk_np(dev);
	if (!priv->scmi_clk_np) {
		// FIXME Fallback to hardware driver?
		return -ENODEV;
	}
dev_dbg(dev, "SCMI clock node is %pOF\n", priv->scmi_clk_np);

	info = of_device_get_match_data(dev);

	// FIXME Check SCMI version to select map table
	// FIXME How to get scmi_revision_info?
	// u16 major_ver;
	// u16 minor_ver;
	// u8 num_protocols;
	// u8 num_agents;
	// u32 impl_ver;
	// char vendor_id[SCMI_SHORT_NAME_MAX_SIZE];
	// char sub_vendor_id[SCMI_SHORT_NAME_MAX_SIZE];
	// FIXME scmi_revision_area_get() takes an scmi_protocol_handle
	// FIXME How to get scmi_protocol_handle?
	priv->map = info->map;

	// FIXME We cannot do lazy look-up in r8a78000_clk_get(), as that
	// FIXME function is called with of_clk_mutex is already held.
	ret = prefill_scmi_clks(priv, info->map);
	if (ret)
		return ret;

	ret = register_fixed_clks(priv);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(dev, r8a78000_clk_get, priv);
}

static /* const */ struct clk_map r8a78000_map_scmi_v2_1[] /* __initconst */ = {
	{ R8A78000_CPG_SGASYNCD4_PERW_BUS,	FIXED_CLK(266M) },
	{ R8A78000_CPG_SGASYNCD16_PERW_BUS,	FIXED_CLK(66M) },
	{ R8A78000_CPG_MSOCK_PERW_BUS,		1671 },
	{ -1 }
};

static const struct r8a78000_cpg_info r8a78000_cpg_info /* __initconst */ = {
	// FIXME Support multiple firmware versions
	.map = r8a78000_map_scmi_v2_1,
};

static const struct of_device_id r8a78000_cpg_match[] = {
	{
		.compatible = "renesas,r8a78000-cpg",
		.data = &r8a78000_cpg_info,
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

// FIXME builtin_platform_driver_probe does not support probe deferral
//builtin_platform_driver_probe(r8a78000_cpg_driver, r8a78000_cpg_probe);
builtin_platform_driver(r8a78000_cpg_driver)

MODULE_DESCRIPTION("R-Car X5H CPG Driver");
