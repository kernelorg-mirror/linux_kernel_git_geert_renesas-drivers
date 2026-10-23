// SPDX-License-Identifier: GPL-2.0
/*
 * R-Car X5H Module Controller
 *
 * Copyright (C) 2026 Glider bv
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/dev_printk.h>
#include <linux/device-id/of.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_clock.h>
#include <linux/pm_domain.h>
#include <linux/reset-controller.h>
#include <linux/reset.h>
#include <linux/scmi_protocol.h>
#include <linux/slab.h>

#include <dt-bindings/power/renesas,r8a78000-mdlc.h>

struct power_id_map_entry {
	u16 hw_id;		/* Hardware power domain ID */
	u16 fw_id;		/* SCMI firmware power domain ID */
};

struct power_id_map {
	const struct power_id_map_entry *entries;
	unsigned int size;
};

#define POWER_ID_MAP(_entries) .power_map = &(struct power_id_map) {	\
	.entries = _entries,						\
	.size = ARRAY_SIZE(_entries),					\
}

struct mod_id_map_entry {
	u16 hw_id;		/* Hardware module ID */
	s16 fw_id;		/* SCMI clock (= reset) ID or -1 */
	s16 fw_fck_parent;	/* SCMI functional clock parent ID or -1 */
};

struct mod_id_map {
	const struct mod_id_map_entry *entries;
	unsigned int size;
};

#define MOD_ID_MAP(_entries) .mod_map = &(struct mod_id_map) {		\
	.entries = _entries,						\
	.size = ARRAY_SIZE(_entries),					\
}

struct mdlc_info {
	u32 base;
	const struct power_id_map *power_map;
	const struct mod_id_map *mod_map;
};

struct mdlc_data {
	const struct mdlc_info *default_info;
	const struct mdlc_info *fw_info;
};

/**
 * struct r8a78000_mdlc_priv - Module Controller Private Data
 *
 * @link: Link into list of MDLC instances
 * @genpd_data: PM domain provider data
 * @rcdev: Reset controller entity
 * @dev: MDLC device
 * @np: Device node in DT representing the MDLC
 * @scmi_clk_np: Device node in DT for the SCMI firmware clock protocol
 * @power_map: Mapping from hardware power domain IDs to SCMI power domain IDs
 * @mod_map: Mapping from hardware module IDs to SCMI clock and reset IDs
 * @genpds: Power domains, indexed by @power_map entry index
 * @rstcs: Reset controls, indexed by @mod_map entry index
 */
struct r8a78000_mdlc_priv {
	struct hlist_node link;
	struct genpd_onecell_data genpd_data;
	struct reset_controller_dev rcdev;
	struct device *dev;
	struct device_node *np;
	struct device_node *scmi_clk_np;
	const struct power_id_map *power_map;
	const struct mod_id_map *mod_map;
	struct generic_pm_domain **genpds;
	struct reset_control **rstcs;
};

static struct generic_pm_domain *r8a78000_genpd_always_on;

static HLIST_HEAD(r8a78000_mdlc_list);
static DEFINE_MUTEX(r8a78000_mdlc_lock);	/* protects the list above */

static int power_id_map_find(const struct power_id_map *map, u32 hw_id)
{
	if (!map)
		return -1;

	for (unsigned int i = 0; i < map->size; i++) {
		if (map->entries[i].hw_id == hw_id)
			return i;
	}

	return -1;
}

static struct generic_pm_domain *r8a78000_genpd_xlate(
			const struct of_phandle_args *spec, void *data)
{
	struct r8a78000_mdlc_priv *priv = container_of(data,
					struct r8a78000_mdlc_priv, genpd_data);
	struct generic_pm_domain *genpd;
	struct device *dev = priv->dev;
	int idx;
	u32 id;

	if (spec->args_count != 2)
		return ERR_PTR(-EINVAL);

	id = spec->args[0];

	if (id >= R8A78000_MDLC_PD_AON) {
		genpd = r8a78000_genpd_always_on;
		goto found;
	}

	idx = power_id_map_find(priv->power_map, id);
	if (idx < 0) {
		dev_err(dev, "Unknown power domain 0x%x\n", id);
		return ERR_PTR(-ENOENT);
	}

	genpd = priv->genpds[idx];

found:
	dev_dbg(dev, "Mapping HW power domain 0x%x to %s\n", id, genpd->name);

	return genpd;
}

#define rcdev_to_priv(_rcdev)	\
	container_of(_rcdev, struct r8a78000_mdlc_priv, rcdev)

static int mod_id_map_find(const struct mod_id_map *map, u32 hw_id)
{
	for (unsigned int i = 0; i < map->size; i++) {
		if (map->entries[i].hw_id == hw_id)
			return i;
	}

	return -1;
}

static int r8a78000_mdlc_reset_xlate(struct reset_controller_dev *rcdev,
				     const struct of_phandle_args *spec)
{
	struct r8a78000_mdlc_priv *priv = rcdev_to_priv(rcdev);
	struct device *dev = priv->dev;
	int idx;
	u32 id;

	if (spec->args_count != 1)
		return -EINVAL;

	id = spec->args[0];

	idx = mod_id_map_find(priv->mod_map, id);
	if (idx < 0) {
		dev_err(dev, "Unknown reset 0x%x\n", id);
		return -ENOENT;
	}

	if (!priv->rstcs[idx])
		dev_dbg(dev, "Ignoring HW reset 0x%x\n", id);
	else
		dev_dbg(dev, "Mapping HW reset 0x%x to SCMI reset %d\n", id,
			priv->mod_map->entries[idx].fw_id);

	return idx;
}

#define DEFINE_MDLC_RESET_WRAPPER(_op)					     \
	static int r8a78000_mdlc_ ## _op(struct reset_controller_dev *rcdev, \
					 unsigned long id)		     \
	{								     \
		struct r8a78000_mdlc_priv *priv = rcdev_to_priv(rcdev);	     \
									     \
		return reset_control_ ## _op(priv->rstcs[id]);		     \
	}

DEFINE_MDLC_RESET_WRAPPER(reset)
DEFINE_MDLC_RESET_WRAPPER(assert)
DEFINE_MDLC_RESET_WRAPPER(deassert)
DEFINE_MDLC_RESET_WRAPPER(status)

static const struct reset_control_ops r8a78000_mdlc_reset_ops = {
	.reset = r8a78000_mdlc_reset,
	.assert = r8a78000_mdlc_assert,
	.deassert = r8a78000_mdlc_deassert,
	.status = r8a78000_mdlc_status,
};

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

static struct clk *scmi_get_clk(struct device *dev, struct device_node *np,
				s16 fw_id)
{
	struct of_phandle_args scmi_spec = {
		.np = np,
		.args_count = 1,
		.args[0] = fw_id,
	};
	struct clk *clk;

	if (fw_id < 0)
		return NULL;

	clk = of_clk_get_from_provider(&scmi_spec);
	if (IS_ERR(clk))
		dev_err(dev, "Cannot get SCMI clock %d: %pe\n", fw_id, clk);
	else
		dev_dbg(dev, "SCMI clock %d is %pC\n", fw_id, clk);

	return clk;
}

static int r8a78000_mdlc_attach_dev(struct generic_pm_domain *domain,
				    struct device *dev)
{
	struct device_node *np = dev->of_node;
	const struct mod_id_map_entry *entry;
	struct r8a78000_mdlc_priv *priv;
	struct of_phandle_args pd_spec;
	struct clk *clk, *parent;
	int ret, idx;
	u32 id;

	ret = of_parse_phandle_with_args(np, "power-domains",
					 "#power-domain-cells", 0, &pd_spec);
	if (ret < 0)
		return ret;

	scoped_guard(mutex, &r8a78000_mdlc_lock) {
		hlist_for_each_entry(priv, &r8a78000_mdlc_list, link) {
			if (priv->np == pd_spec.np)
				break;
		}
	}

	if (!priv) {
		dev_warn(dev, "%s: Ignoring non-MDLC %pOF\n", __func__,
			 pd_spec.np);
		of_node_put(pd_spec.np);
		return 0;
	}

	id = pd_spec.args[1];
	of_node_put(pd_spec.np);

	idx = mod_id_map_find(priv->mod_map, id);
	if (idx < 0) {
		dev_err(dev, "Unknown module 0x%x\n", id);
		return -ENOENT;
	}

	if (!priv->scmi_clk_np) {
		dev_dbg(dev, "Ignoring HW module 0x%x\n", id);
		return 0;
	}

	entry = &priv->mod_map->entries[idx];
	dev_dbg(dev, "Mapping HW module 0x%x to SCMI clock %d fck parent %d\n",
		id, entry->fw_id, entry->fw_fck_parent);

	clk = scmi_get_clk(dev, priv->scmi_clk_np, entry->fw_id);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	parent = scmi_get_clk(dev, priv->scmi_clk_np, entry->fw_fck_parent);
	if (IS_ERR(parent)) {
		ret = PTR_ERR(parent);
		goto fail_put_clk;
	}

	if (!clk && !parent) {
		/* Ignore missing SCMI clocks */
		return 0;
	}

	ret = pm_clk_create(dev);
	if (ret)
		goto fail_put_parent;

	if (parent) {
		ret = pm_clk_add_clk(dev, parent);
		if (ret) {
			pm_clk_destroy(dev);
			goto fail_put_parent;
		}
	}
	if (clk) {
		ret = pm_clk_add_clk(dev, clk);
		if (ret) {
			pm_clk_destroy(dev);
			goto fail_put_clk;
		}
	}
	return 0;

fail_put_parent:
	clk_put(parent);
fail_put_clk:
	clk_put(clk);
	return ret;
}

static void r8a78000_mdlc_detach_dev(struct generic_pm_domain *domain,
				     struct device *dev)
{
	if (!pm_clk_no_clocks(dev))
		pm_clk_destroy(dev);
}

static struct generic_pm_domain **get_genpds(struct r8a78000_mdlc_priv *priv,
					     struct device_node *scmi_power_np)
{
	const struct power_id_map *map = priv->power_map;
	struct generic_pm_domain **genpds, *genpd;
	struct of_phandle_args scmi_spec;
	struct device *dev = priv->dev;
	u16 fw_id;

	if (!map)
		return NULL;

	genpds = devm_kzalloc_objs(dev, *genpds, map->size);
	if (!genpds)
		return ERR_PTR(-ENOMEM);

	for (unsigned int i = 0; i < map->size; i++) {
		fw_id = map->entries[i].fw_id;

		scmi_spec.np = scmi_power_np;
		scmi_spec.args_count = 1;
		scmi_spec.args[0] = fw_id;

		genpd = of_genpd_get_from_provider(&scmi_spec);
		if (IS_ERR(genpd))
			return dev_err_cast_probe(dev, genpd,
					"Failed to get SCMI power domain %u\n",
					fw_id);

		dev_dbg(dev, "SCMI power domain %u is %s\n", fw_id,
			genpd->name);

		genpds[i] = genpd;

		/* Hook up clock domain support */
		genpd->attach_dev = r8a78000_mdlc_attach_dev;
		genpd->detach_dev = r8a78000_mdlc_detach_dev;
		/* Setting flags this late has no impact, but does not hurt */
		genpd->flags |= GENPD_FLAG_PM_CLK;
		genpd->dev_ops.stop = pm_clk_suspend;
		genpd->dev_ops.start = pm_clk_resume;
	}

	return genpds;
}

static struct reset_control **get_rstcs(struct r8a78000_mdlc_priv *priv,
					struct fwnode_handle *scmi_reset_fwnode)
{
	const struct mod_id_map *map = priv->mod_map;
	struct fwnode_reference_args scmi_spec;
	struct reset_control **rstcs, *rstc;
	struct device *dev = priv->dev;
	s16 fw_id;

	if (!map)
		return NULL;

	rstcs = devm_kzalloc_objs(dev, *rstcs, map->size);
	if (!rstcs)
		return ERR_PTR(-ENOMEM);

	for (unsigned int i = 0; i < map->size; i++) {
		fw_id = map->entries[i].fw_id;
		if (!scmi_reset_fwnode || fw_id < 0)
			continue;

		scmi_spec.fwnode = scmi_reset_fwnode;
		scmi_spec.nargs = 1;
		scmi_spec.args[0] = fw_id;

		rstc = reset_control_get_from_provider_exclusive(&scmi_spec);
		if (IS_ERR(rstc)) {
			while (i--)
				reset_control_put(rstcs[i]);

			return dev_err_cast_probe(dev, rstc,
					"Failed to get SCMI reset %u\n", fw_id);
		}

		rstcs[i] = rstc;
	}

	return rstcs;
}

static void r8a78000_mdlc_unlink(void *data)
{
	struct r8a78000_mdlc_priv *priv = data;

	scoped_guard(mutex, &r8a78000_mdlc_lock) {
		hlist_del(&priv->link);
	}
}

static void r8a78000_genpd_del_provider(void *data)
{
	of_genpd_del_provider(data);
}

static int r8a78000_genpd_always_on_singleton(struct device *dev)
{
	static DEFINE_MUTEX(singleton_lock);
	struct generic_pm_domain *genpd;
	int ret;

	guard(mutex)(&singleton_lock);

	if (r8a78000_genpd_always_on)
		return 0;

	genpd = kzalloc_obj(*genpd);
	if (!genpd)
		return -ENOMEM;

	genpd->name = "always-on";
	genpd->attach_dev = r8a78000_mdlc_attach_dev;
	genpd->detach_dev = r8a78000_mdlc_detach_dev;
	genpd->flags |= GENPD_FLAG_PM_CLK;

	ret = pm_genpd_init(genpd, &pm_domain_always_on_gov, false);
	if (ret) {
		kfree(genpd);
		return dev_err_probe(dev, ret,
				     "Failed to create always-on domain\n");
	}

	r8a78000_genpd_always_on = genpd;
	return 0;
}

static int r8a78000_mdlc_probe(struct platform_device *pdev)
{
	struct device_node *scmi_reset_np __free(device_node) = NULL;
	struct device_node *scmi_power_np __free(device_node) = NULL;
	struct device_node *scmi_clk_np __free(device_node) = NULL;
	struct device_node *scmi __free(device_node) = NULL;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct mdlc_data *mdlc_data;
	struct r8a78000_mdlc_priv *priv;
	const struct mdlc_info *info;
	struct resource *res;
	int ret;

	ret = r8a78000_genpd_always_on_singleton(dev);
	if (ret)
		return ret;

	mdlc_data = of_device_get_match_data(dev);
	if (!mdlc_data)
		return -ENODEV;

	info = mdlc_data->default_info;

	priv = devm_kzalloc_obj(dev, *priv);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->np = np;

	if (!IS_ENABLED(CONFIG_ARM_SCMI_POWER_DOMAIN) ||
	    !IS_ENABLED(CONFIG_COMMON_CLK_SCMI) ||
	    !IS_ENABLED(CONFIG_RESET_SCMI)) {
		dev_dbg(dev, "SCMI pmdomain/clock/reset support is disabled\n");
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

	scmi_power_np = scmi_find_proto(scmi, SCMI_PROTOCOL_POWER);
	if (!scmi_power_np) {
		dev_dbg(dev,
			"Cannot find SCMI power domain management protocol\n");
		goto fallback;
	}

	scmi_clk_np = scmi_find_proto(scmi, SCMI_PROTOCOL_CLOCK);
	if (!scmi_clk_np) {
		dev_dbg(dev, "Cannot find SCMI clock management protocol\n");
		goto fallback;
	}

	scmi_reset_np = scmi_find_proto(scmi, SCMI_PROTOCOL_RESET);
	if (!scmi_reset_np) {
		dev_dbg(dev, "Cannot find SCMI reset management protocol\n");
		goto fallback;
	}

	priv->scmi_clk_np = scmi_clk_np;
	info = mdlc_data->fw_info;

fallback:
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	for (; info->base; info++) {
		if (info->base == res->start)
			break;
	}

	if (!info->base) {
		dev_dbg(dev, "Unsupported MDLC instance 0x%pa\n", &res->start);
		return -ENODEV;
	}

	priv->power_map = info->power_map;
	priv->mod_map = info->mod_map;

	/*
	 * We cannot do lazy look-up in r8a78000_genpd_xlate(), as that
	 * function is called with of_genpd_mutex already held.
	 */
	priv->genpds = get_genpds(priv, scmi_power_np);
	if (IS_ERR(priv->genpds))
		return PTR_ERR(priv->genpds);

	/*
	 * We cannot do lazy look-up in r8a78000_mdlc_reset_xlate(), as that
	 * function is called with reset_list_mutex already held.
	 */
	priv->rstcs = get_rstcs(priv, of_fwnode_handle(scmi_reset_np));
	if (IS_ERR(priv->rstcs))
		return PTR_ERR(priv->rstcs);

	scoped_guard(mutex, &r8a78000_mdlc_lock) {
		hlist_add_head(&priv->link, &r8a78000_mdlc_list);
	}

	ret = devm_add_action_or_reset(dev, r8a78000_mdlc_unlink, priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add action\n");

	/* Note that no actual domains are registered, just need translation */
	priv->genpd_data.xlate = r8a78000_genpd_xlate;
	ret = of_genpd_add_provider_onecell(np, &priv->genpd_data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register genpd provider\n");

	ret = devm_add_action_or_reset(dev, r8a78000_genpd_del_provider, np);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to add unregister action\n");

	priv->rcdev.ops = &r8a78000_mdlc_reset_ops;
	priv->rcdev.of_node = np;
	priv->rcdev.of_reset_n_cells = 1;
	priv->rcdev.of_xlate = r8a78000_mdlc_reset_xlate;

	ret = devm_reset_controller_register(dev, &priv->rcdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register reset controller\n");

	/* Retained in priv->scmi_clk_np */
	retain_and_null_ptr(scmi_clk_np);

	return 0;
}

static const struct mod_id_map_entry r8a78000_mdlc_aon_mod_default[] = {
	{ 0x20, -1, -1 },	/* GPIODM0 */
};

static const struct mod_id_map_entry r8a78000_mdlc_hscn_mod_default[] = {
	{ 0x50, -1, -1 },	/* PFC_HSCN_GPIODM0 */
};

static const struct mod_id_map_entry r8a78000_mdlc_pere_mod_default[] = {
	{ 0x30, -1, -1 },	/* PFC_PERE_GPIODM0 */
};

static const struct mod_id_map_entry r8a78000_mdlc_perw_mod_default[] = {
	{ 0x30, -1, -1 },	/* PFC_PERW_GPIODM0 */
	{ 0x54, -1, -1 },	/* HSCIF0 */
};

static const struct mdlc_info r8a78000_mdlc_default[] = {
	{
		.base = 0xc1338000,
		MOD_ID_MAP(r8a78000_mdlc_aon_mod_default),
	}, {
		.base = 0xc9c90000,
		MOD_ID_MAP(r8a78000_mdlc_hscn_mod_default),
	}, {
		.base = 0xc08f0000,
		MOD_ID_MAP(r8a78000_mdlc_pere_mod_default),
	}, {
		.base = 0xc05d0000,
		MOD_ID_MAP(r8a78000_mdlc_perw_mod_default),
	},
	{ /* sentinel */ }
};

static const struct mod_id_map_entry r8a78000_mdlc_aon_mod_fw[] = {
	{ 0x20, 811, -1 },	/* GPIODM0 */
};

static const struct mod_id_map_entry r8a78000_mdlc_hscn_mod_fw[] = {
	{ 0x50, 332, -1 },	/* PFC_HSCN_GPIODM0 */
};

static const struct mod_id_map_entry r8a78000_mdlc_pere_mod_fw[] = {
	{ 0x30, 193, -1 },	/* PFC_PERE_GPIODM0 */
};

static const struct mod_id_map_entry r8a78000_mdlc_perw_mod_fw[] = {
	{ 0x30, 201, -1 },	/* PFC_PERW_GPIODM0 */
	{ 0x40, 205, -1 },	/* SCIF0 */
	{ 0x41, 206, -1 },	/* SCIF1 */
	{ 0x42, 207, -1 },	/* SCIF3 */
	{ 0x43, 208, -1 },	/* SCIF4 */
	{ 0x44, 209, -1 },	/* I2C1 */
	{ 0x45, 210, -1 },	/* I2C2 */
	{ 0x46, 211, -1 },	/* I2C3 */
	{ 0x47, 212, -1 },	/* I2C4 */
	{ 0x48, 213, -1 },	/* I2C5 */
	{ 0x49, 214, -1 },	/* I2C6 */
	{ 0x4a, 215, -1 },	/* I2C7 */
	{ 0x4b, 216, -1 },	/* I2C8 */
	{ 0x4f, 220, 1667 },	/* MSI4 / MSOCK_PERW_BUS */
	{ 0x50, 221, 1667 },	/* MSI5 / MSOCK_PERW_BUS */
	{ 0x51, 222, 1667 },	/* MSI6 / MSOCK_PERW_BUS */
	{ 0x52, 223, 1667 },	/* MSI7 / MSOCK_PERW_BUS */
	{ 0x54, 224, -1 },	/* HSCIF0 */
	{ 0x55, 225, -1 },	/* HSCIF1 */
	{ 0x56, 226, -1 },	/* HSCIF2 */
	{ 0x57, 227, -1 },	/* HSCIF3 */
	{ 0x70, 244, -1 },	/* PWM0 */
	{ 0x76, 249, -1 },	/* TPU0 */
};

static const struct mod_id_map_entry r8a78000_mdlc_scp_mod_fw[] = {
	{ 0x50, 776, 1459 },	/* MSI0 / MSO_SCP_MAIN */
	{ 0x51, 777, 1459 },	/* MSI1 / MSO_SCP_MAIN */
	{ 0x52, 778, 1459 },	/* MSI2 / MSO_SCP_MAIN */
	{ 0x53, 779, 1459 },	/* MSI3 / MSO_SCP_MAIN */
};

static const struct mdlc_info r8a78000_mdlc_fw[] = {
	{
		.base = 0xc1338000,
		MOD_ID_MAP(r8a78000_mdlc_aon_mod_fw),
	}, {
		.base = 0xc9c90000,
		MOD_ID_MAP(r8a78000_mdlc_hscn_mod_fw),
	}, {
		.base = 0xc08f0000,
		MOD_ID_MAP(r8a78000_mdlc_pere_mod_fw),
	}, {
		.base = 0xc05d0000,
		MOD_ID_MAP(r8a78000_mdlc_perw_mod_fw),
	}, {
		.base = 0xc1330000,
		MOD_ID_MAP(r8a78000_mdlc_scp_mod_fw),
	},
	{ /* sentinel */ }
};

static const struct mdlc_data r8a78000_mdlc_data = {
	.default_info = r8a78000_mdlc_default,
	.fw_info = r8a78000_mdlc_fw,
};

static const struct of_device_id r8a78000_mdlc_match[] = {
	{
		.compatible = "renesas,r8a78000-mdlc",
		.data = &r8a78000_mdlc_data,
	},
	{ /* sentinel */ }
};

static struct platform_driver r8a78000_mdlc_driver = {
	.probe = r8a78000_mdlc_probe,
	.driver = {
		.name = "r8a78000-mdlc",
		.of_match_table = r8a78000_mdlc_match,
		.suppress_bind_attrs = true,
	},
};

builtin_platform_driver(r8a78000_mdlc_driver)

MODULE_DESCRIPTION("R-Car X5H MDLC Driver");
