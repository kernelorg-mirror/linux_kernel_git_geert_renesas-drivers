// SPDX-License-Identifier: GPL-2.0
/*
 * r8a78000 Module Controller
 *
 * Copyright (C) 2026 Glider bv
 */

#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <linux/pm_clock.h>
#include <linux/pm_domain.h>
#include <linux/reset-controller.h>
#include <linux/scmi_protocol.h>

#include <dt-bindings/power/renesas,r8a78000-mdlc.h>

struct power_map {
	int hw_id;		/* Hardware power domain ID */
	u32 scmi_id;		/* SCMI power domain ID */
	struct generic_pm_domain *genpd;
};

struct mod_map {
	int hw_id;		/* Hardware module ID */
	u32 scmi_id;		/* SCMI clock and reset IDs are identical */
};

struct r8a78000_mdlc_info {
	u32 base;
	/* FIXME const */ struct power_map *power_map;
	const struct mod_map *mod_map;
};

/**
 * struct r8a78000_mdlc_priv - Module Controller Private Data
 *
 * @link: Link into list of MDLC instances
 * @genpd_data: PM domain provider data
 * @rcdev: Reset controller entity
 * @dev: MDLC device
 * @np: Device node in DT representing the MDLC
 * @scmi_power_np: Device node in DT for the SCMI firmware power protocol
 * @scmi_clk_np: Device node in DT for the SCMI firmware clock protocol
 * @scmi_reset_np: Device node in DT for the SCMI firmware reset protocol
 * @scmi_rcdev: SCMI reset controller entity
 * @power_map: Mapping from hardware power domain IDs to SCMI power domains
 * @mod_map: Mapping from hardware module IDs to SCMI clocks and resets
 */
struct r8a78000_mdlc_priv {
	struct hlist_node link;
	struct genpd_onecell_data genpd_data;
	struct reset_controller_dev rcdev;
	struct device *dev;
	struct device_node *np;
	struct device_node *scmi_power_np;
	struct device_node *scmi_clk_np;
	struct device_node *scmi_reset_np;
	struct reset_controller_dev *scmi_rcdev;
	const struct power_map *power_map;
	const struct mod_map *mod_map;
};

static struct generic_pm_domain *r8a78000_genpd_always_on;
static HLIST_HEAD(r8a78000_mdlc_list);
static DEFINE_MUTEX(r8a78000_mdlc_lock);	/* protects the two above */

static const struct power_map *power_map_find(const struct power_map *map,
					      u32 id)
{
	if (!map)
		return NULL;

	for (; map->hw_id >= 0; map++) {
		if (map->hw_id == id)
			return map;
	}

	return NULL;
}

static struct generic_pm_domain *r8a78000_genpd_xlate(
			const struct of_phandle_args *spec, void *data)
{
	struct r8a78000_mdlc_priv *priv = container_of(data,
					struct r8a78000_mdlc_priv, genpd_data);
	struct generic_pm_domain *genpd;
	struct device *dev = priv->dev;
	const struct power_map *map;
	u32 id;

	if (spec->args_count != 2)
		return ERR_PTR(-EINVAL);

	id = spec->args[0];

	if (id >= R8A78000_MDLC_PD_AON) {
dev_info(dev, "Mapping power domain %u to always-on domain\n", id);
		return r8a78000_genpd_always_on;
	}

	map = power_map_find(priv->power_map, id);
	if (!map) {
		dev_err(dev, "Unknown power domain %u\n", id);
		return ERR_PTR(-ENOENT);
	}

dev_info(dev, "Mapping power domain %u to SCMI power domain %u\n", id, map->scmi_id);

	genpd = map->genpd;

	// FIXME dev_dbg
	dev_info(dev, "Power domain %u is %s\n", id, genpd->name);

	return genpd;
}

#define rcdev_to_priv(_rcdev)	\
	container_of(_rcdev, struct r8a78000_mdlc_priv, rcdev)

static const struct mod_map *mod_map_find(const struct mod_map *map, u32 id)
{
	if (!map)
		return NULL;

	for (; map->hw_id >= 0; map++) {
		if (map->hw_id == id)
			return map;
	}

	return NULL;
}

static int r8a78000_mdlc_reset_xlate(struct reset_controller_dev *rcdev,
				     const struct of_phandle_args *spec)
{
	struct r8a78000_mdlc_priv *priv = rcdev_to_priv(rcdev);
	struct device *dev = priv->dev;
	const struct mod_map *map;
	u32 id;

	if (spec->args_count != 1)
		return -EINVAL;

	id = spec->args[0];

	map = mod_map_find(priv->mod_map, id);
	if (!map) {
		dev_err(dev, "Unknown reset %u\n", id);
		return -ENOENT;
	}

dev_info(dev, "Mapping reset %u to SCMI reset %u\n", id, map->scmi_id);

	return map->scmi_id;
}

#define DEFINE_RESET_WRAPPER(op)					    \
	static int r8a78000_mdlc_ ## op(struct reset_controller_dev *rcdev, \
					unsigned long id)		    \
	{								    \
		struct r8a78000_mdlc_priv *priv = rcdev_to_priv(rcdev);	    \
									    \
dev_info(priv->dev, "%s: id %lu\n", __func__, id);			    \
		if (!priv->scmi_rcdev->ops->op)				    \
			return -ENOTSUPP;				    \
									    \
		return priv->scmi_rcdev->ops->op(priv->scmi_rcdev, id);     \
	}

DEFINE_RESET_WRAPPER(reset)
DEFINE_RESET_WRAPPER(assert)
DEFINE_RESET_WRAPPER(deassert)
DEFINE_RESET_WRAPPER(status)

static const struct reset_control_ops r8a78000_mdlc_reset_ops = {
	.reset = r8a78000_mdlc_reset,
	.assert = r8a78000_mdlc_assert,
	.deassert = r8a78000_mdlc_deassert,
	.status = r8a78000_mdlc_status,
};

static struct device_node *scmi_find_proto(struct device_node *scmi, u32 proto)
{
	for_each_available_child_of_node_scoped(scmi, child) {
		u32 x;

		if (of_property_read_u32(child, "reg", &x))
			continue;

		if (x == proto)
			return_ptr(child);
	}

	return NULL;
}

static int r8a78000_mdlc_attach_dev(struct generic_pm_domain *domain,
				    struct device *dev)
{
	struct r8a78000_mdlc_priv *priv;
	struct of_phandle_args pd_spec, scmi_spec;
	struct device_node *np = dev->of_node;
	const struct mod_map *map;
	unsigned int id;
	struct clk *clk;
	int ret;

dev_info(dev, "%s: domain %s\n", __func__, domain->name);
	ret = of_parse_phandle_with_args(np, "power-domains",
					 "#power-domain-cells", 0, &pd_spec);
	if (ret < 0)
		return ret;

	if (pd_spec.args_count != 2) {
		of_node_put(pd_spec.np);
		return -EINVAL;
	}

	scoped_guard(mutex, &r8a78000_mdlc_lock) {
		hlist_for_each_entry(priv, &r8a78000_mdlc_list, link) {
			if (priv->np == pd_spec.np)
				break;
		}
	}

	if (!priv) {
dev_err(dev, "%s: mdlc %pOF not found\n", __func__, pd_spec.np);
		of_node_put(pd_spec.np);
		return -ENODEV;
	}

	id = pd_spec.args[1];
	of_node_put(pd_spec.np);

	map = mod_map_find(priv->mod_map, id);
	if (!map) {
		dev_err(dev, "Unknown module %u\n", id);
		return -ENOENT;
	}

dev_info(dev, "Mapping module %u to SCMI clock %u\n", id, map->scmi_id);

	scmi_spec.np = priv->scmi_clk_np;
	scmi_spec.args_count = 1;
	scmi_spec.args[0] = map->scmi_id;

	clk = of_clk_get_from_provider(&scmi_spec);
	if (IS_ERR(clk)) {
		dev_err(dev, "Cannot get SCMI clock %u: %pe\n", map->scmi_id,
			clk);
		return PTR_ERR(clk);
	}

	// FIXME dev_dbg
	dev_info(dev, "SCMI clock %u is %pC\n", map->scmi_id, clk);

	ret = pm_clk_create(dev);
	if (ret)
		goto fail_put;

	ret = pm_clk_add_clk(dev, clk);
	if (ret)
		goto fail_destroy;

	return 0;

fail_destroy:
	pm_clk_destroy(dev);
fail_put:
	clk_put(clk);
	return ret;
}

static void r8a78000_mdlc_detach_dev(struct generic_pm_domain *domain,
				     struct device *dev)
{
dev_info(dev, "%s: domain %s\n", __func__, domain->name);
	if (!pm_clk_no_clocks(dev))
		pm_clk_destroy(dev);
}

static int r8a78000_mdlc_stop(struct device *dev)
{
dev_info(dev, "%s\n", __func__);
	return pm_clk_suspend(dev);
}

static int r8a78000_mdlc_start(struct device *dev)
{
dev_info(dev, "%s\n", __func__);
	return pm_clk_resume(dev);
}

static int scmi_prefill_power(struct r8a78000_mdlc_priv *priv,
			      struct power_map *map)
{
	struct of_phandle_args scmi_spec;
	struct generic_pm_domain *genpd;

	if (!map)
		return 0;

	for (; map->hw_id >= 0; map++) {

		scmi_spec.np = priv->scmi_power_np;
		scmi_spec.args_count = 1;
		scmi_spec.args[0] = map->scmi_id;

		genpd = genpd_get_from_provider(&scmi_spec);
		if (IS_ERR(genpd))
			return dev_err_probe(priv->dev, PTR_ERR(genpd),
					"Failed to get SCMI power domain %u\n",
					map->scmi_id);

dev_info(priv->dev, "SCMI power domain %u is %s\n", map->scmi_id, genpd->name);

		map->genpd = genpd;

		/* Hook up clock domain support */
		genpd->attach_dev = r8a78000_mdlc_attach_dev;
		genpd->detach_dev = r8a78000_mdlc_detach_dev;
		/* Setting flags this late has no impact, but does not hurt */
		genpd->flags |= GENPD_FLAG_PM_CLK;
		genpd->dev_ops.stop = r8a78000_mdlc_stop;
		genpd->dev_ops.start = r8a78000_mdlc_start;
	}

	return 0;
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
	struct generic_pm_domain *genpd;
	int ret;

	guard(mutex)(&r8a78000_mdlc_lock);

	if (r8a78000_genpd_always_on)
		return 0;

	genpd = kzalloc(sizeof(*genpd), GFP_KERNEL);
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

static int /* __init */ r8a78000_mdlc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *scmi __free(device_node);
	const struct r8a78000_mdlc_info *info;
	struct r8a78000_mdlc_priv *priv;
	struct resource *res;
	int ret;

	ret = r8a78000_genpd_always_on_singleton(dev);
	if (ret)
		return ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->np = np;

	scmi = of_find_compatible_node(NULL, NULL, "arm,scmi");
	if (!scmi) {
		dev_err(dev, "Cannot find SCMI firmware node\n");
		return -ENODEV;
	}

	priv->scmi_power_np = scmi_find_proto(scmi, SCMI_PROTOCOL_POWER);
	if (!priv->scmi_power_np) {
		// FIXME Fallback to hardware driver?
		dev_err(dev,
			"Cannot find SCMI power domain management protocol\n");
		return -ENODEV;
	}
dev_dbg(dev, "SCMI power node is %pOF\n", priv->scmi_power_np);

	priv->scmi_clk_np = scmi_find_proto(scmi, SCMI_PROTOCOL_CLOCK);
	if (!priv->scmi_clk_np) {
		// FIXME Fallback to hardware driver?
		dev_err(dev, "Cannot find SCMI clock management protocol\n");
		return -ENODEV;
	}
dev_dbg(dev, "SCMI clock node is %pOF\n", priv->scmi_clk_np);

	priv->scmi_reset_np = scmi_find_proto(scmi, SCMI_PROTOCOL_RESET);
	if (!priv->scmi_reset_np) {
		// FIXME Fallback to hardware driver?
		dev_err(dev, "Cannot find SCMI reset management protocol\n");
		return -ENODEV;
	}
dev_dbg(dev, "SCMI reset node is %pOF\n", priv->scmi_reset_np);

	priv->scmi_rcdev = reset_controller_get_provider(priv->scmi_reset_np);
dev_dbg(dev, "scmi_rcdev = %pe\n", priv->scmi_rcdev);
	if (!priv->scmi_rcdev)
		return -EPROBE_DEFER;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	for (info = of_device_get_match_data(dev); info->base; info++) {
		if (info->base == res->start)
			break;
	}

	if (!info->base) {
		dev_err(dev, "Unknown MDLC instance 0x%pa\n", &res->start);
		return -ENODEV;
	}

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
	priv->power_map = info->power_map;
	priv->mod_map = info->mod_map;

	// FIXME We cannot do lazy look-up in r8a78000_genpd_xlate(), as that
	// FIXME function is called with of_genpd_mutex already held.
	ret = scmi_prefill_power(priv, info->power_map);
	if (ret)
		return ret;

	scoped_guard(mutex, &r8a78000_mdlc_lock) {
		hlist_add_head(&priv->link, &r8a78000_mdlc_list);
	}

	ret = devm_add_action_or_reset(dev, r8a78000_mdlc_unlink, priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add action\n");

	// FIXME genpd_add_provider() would be sufficient, but is private
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

	return 0;
}

// FIXME We don't need all of them from the start; only add when used/tested
static /* const */ struct power_map r8a78000_mdlc_pere_power_scmi_v2_1[] = {
	{ 0, 12 },	/* PD_UFS0 */
	{ 1, 13 },	/* PD_UFS1 */
	{ -1 },
};

static /* const */ struct mod_map r8a78000_mdlc_pere_mod_scmi_v2_1[] = {
	{ 0x30, 197 },	/* PERE_GPIODM0 */
	// No CLOCK_ATTRIBUTES { 0x31, 198 },	/* PERE_GPIODM1 */
	// No CLOCK_ATTRIBUTES { 0x32, 199 },	/* PERE_GPIODM2 */
	// No CLOCK_ATTRIBUTES { 0x33, 200 },	/* PERE_GPIODM3 */
	{ 0x40, 201 },	/* RPC */
	{ 0x60, 202 },	/* UFS0 */
	{ 0x61, 203 },	/* UFS1 */
	{ 0x70, 204 },	/* SDHI0 */
	{ -1 },
};

static /* const */ struct mod_map r8a78000_mdlc_perw_mod_scmi_v2_1[] = {
	{ 0x30, 205 },	/* PERW_GPIODM0 */
	// No CLOCK_ATTRIBUTES { 0x31, 206 },	/* PERW_GPIODM1 */
	// No CLOCK_ATTRIBUTES { 0x32, 207 },	/* PERW_GPIODM2 */
	// No CLOCK_ATTRIBUTES { 0x33, 208 },	/* PERW_GPIODM3 */
	{ 0x40, 209 },	/* SCIF0 */
	{ 0x41, 210 },	/* SCIF1 */
	{ 0x42, 211 },	/* SCIF3 */
	{ 0x43, 212 },	/* SCIF4 */
	{ 0x44, 213 },	/* I2C1 */
	{ 0x45, 214 },	/* I2C2 */
	{ 0x46, 215 },	/* I2C3 */
	{ 0x47, 216 },	/* I2C4 */
	{ 0x48, 217 },	/* I2C5 */
	{ 0x49, 218 },	/* I2C6 */
	{ 0x4a, 219 },	/* I2C7 */
	{ 0x4b, 220 },	/* I2C8 */
	{ 0x4c, 221 },	/* I3C0 */
	{ 0x4d, 222 },	/* I3C1 */
	{ 0x4e, 223 },	/* I3C2 */
	{ 0x4f, 224 },	/* MSI4 */
	{ 0x50, 225 },	/* MSI5 */
	{ 0x51, 226 },	/* MSI6 */
	{ 0x52, 227 },	/* MSI7 */
	// No CLOCK_ATTRIBUTES { 0x54, 228 },	/* HSCIF0 */
	{ 0x55, 229 },	/* HSCIF1 */
	{ 0x56, 230 },	/* HSCIF2 */
	{ 0x57, 231 },	/* HSCIF3 */
	{ 0x58, 232 },	/* DRI00 */
	{ 0x59, 233 },	/* DRI01 */
	{ 0x5a, 234 },	/* DRI10 */
	{ 0x5b, 235 },	/* DRI11 */
	{ 0x5c, 236 },	/* DRI20 */
	{ 0x5d, 237 },	/* DRI21 */
	{ 0x5e, 238 },	/* DRI30 */
	{ 0x5f, 239 },	/* DRI31 */
	{ 0x60, 240 },	/* DRI40 */
	{ 0x61, 241 },	/* DRI41 */
	{ 0x62, 242 },	/* DRI50 */
	{ 0x63, 243 },	/* DRI51 */
	{ 0x64, 244 },	/* DRI60 */
	{ 0x65, 245 },	/* DRI61 */
	{ 0x66, 246 },	/* DRI70 */
	{ 0x67, 247 },	/* DRI71 */
	{ 0x70, 248 },	/* PWM0 */
	{ 0x72, 249 },	/* TMU1 */
	{ 0x73, 250 },	/* TMU2 */
	{ 0x74, 251 },	/* TMU3 */
	{ 0x75, 252 },	/* TMU4 */
	{ 0x76, 253 },	/* TPU0 */
	{ 0x90, 254 },	/* ADG0 */
	{ 0x91, 255 },	/* ADG1 */
	{ 0x92, 256 },	/* SSI0 */
	{ 0x93, 257 },	/* SSI00 */
	{ 0x94, 258 },	/* SSI01 */
	{ 0x95, 259 },	/* SSI02 */
	{ 0x96, 260 },	/* SSI03 */
	{ 0x97, 261 },	/* SSI04 */
	{ 0x98, 262 },	/* SSI05 */
	{ 0x99, 263 },	/* SSI06 */
	{ 0x9a, 264 },	/* SSI07 */
	{ 0x9b, 265 },	/* SSI08 */
	{ 0x9c, 266 },	/* SSI09 */
	{ 0x9d, 267 },	/* SSI1 */
	{ 0x9e, 268 },	/* SSI10 */
	{ 0x9f, 269 },	/* SSI11 */
	{ 0xa0, 270 },	/* SSI12 */
	{ 0xa1, 271 },	/* SSI13 */
	{ 0xa2, 272 },	/* SSI14 */
	{ 0xa3, 273 },	/* SSI15 */
	{ 0xa4, 274 },	/* SSI16 */
	{ 0xa5, 275 },	/* SSI17 */
	{ 0xa6, 276 },	/* SSI18 */
	{ 0xa7, 277 },	/* SSI19 */
	{ 0xa8, 278 },	/* SCU0 */
	{ 0xa9, 279 },	/* SRC00 */
	{ 0xaa, 280 },	/* SRC01 */
	{ 0xab, 281 },	/* SRC02 */
	{ 0xac, 282 },	/* SRC03 */
	{ 0xad, 283 },	/* SRC04 */
	{ 0xae, 284 },	/* SRC05 */
	{ 0xaf, 285 },	/* SRC06 */
	{ 0xb0, 286 },	/* SRC07 */
	{ 0xb1, 287 },	/* SRC08 */
	{ 0xb2, 288 },	/* SRC09 */
	{ 0xb3, 289 },	/* SCU00 */
	{ 0xb4, 290 },	/* SCU01 */
	{ 0xb5, 291 },	/* DVC00 */
	{ 0xb6, 292 },	/* DVC01 */
	{ 0xb7, 293 },	/* SCU1 */
	{ 0xb8, 294 },	/* SRC10 */
	{ 0xb9, 295 },	/* SRC11 */
	{ 0xba, 296 },	/* SRC12 */
	{ 0xbb, 297 },	/* SRC13 */
	{ 0xbc, 298 },	/* SRC14 */
	{ 0xbd, 299 },	/* SRC15 */
	{ 0xbe, 300 },	/* SRC16 */
	{ 0xbf, 301 },	/* SRC17 */
	{ 0xc0, 302 },	/* SRC18 */
	{ 0xc1, 303 },	/* SRC19 */
	{ 0xc2, 304 },	/* SCU10 */
	{ 0xc3, 305 },	/* SCU11 */
	{ 0xc4, 306 },	/* DVC10 */
	{ 0xc5, 307 },	/* DVC11 */
	{ 0xc6, 308 },	/* APD00 */
	{ 0xc7, 309 },	/* APD01 */
	{ 0xc8, 310 },	/* APD10 */
	{ 0xc9, 311 },	/* APD11 */
	{ 0xca, 312 },	/* APD02 */
	{ 0xcb, 313 },	/* APD12 */
	{ -1 },
};

static const struct r8a78000_mdlc_info r8a78000_mdlc_info[] /* __initconst */ = {
	// FIXME Support multiple firmware versions
	{
		.base = 0xc3060000 /* mdlc_vipn */,
		/* FIXME .power_map = r8a78000_mdlc_vipn_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_vipn_mod_scmi_v2_1, */
	}, {
		.base = 0xc3460000 /* mdlc_vips */,
		/* FIXME .power_map = r8a78000_mdlc_vips_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_vips_mod_scmi_v2_1, */
	}, {
		.base = 0xc5000000 /* mdlc_vio */,
		/* FIXME .power_map = r8a78000_mdlc_vio_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_vio_mod_scmi_v2_1, */
	}, {
		.base = 0xc08f0000 /* mdlc_pere */,
		.power_map = r8a78000_mdlc_pere_power_scmi_v2_1,
		.mod_map = r8a78000_mdlc_pere_mod_scmi_v2_1,
	}, {
		.base = 0xc05d0000 /* mdlc_perw */,
		.mod_map = r8a78000_mdlc_perw_mod_scmi_v2_1,
	}, {
		.base = 0xe8000000 /* mdlc_ddr0 */,
	}, {
		.base = 0xe8080000 /* mdlc_ddr1 */,
	}, {
		.base = 0xe8100000 /* mdlc_ddr2 */,
	}, {
		.base = 0xe8180000 /* mdlc_ddr3 */,
	}, {
		.base = 0xe8200000 /* mdlc_ddr4 */,
	}, {
		.base = 0xe8280000 /* mdlc_ddr5 */,
	}, {
		.base = 0xe8300000 /* mdlc_ddr6 */,
	}, {
		.base = 0xe8380000 /* mdlc_ddr7 */,
	}, {
		.base = 0xc9c90000 /* mdlc_hscn */,
		/* FIXME .power_map = r8a78000_mdlc_hscn_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_hscn_mod_scmi_v2_1, */
	}, {
		.base = 0x19440000 /* mdlc_rt */,
		/* FIXME .power_map = r8a78000_mdlc_rt_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_rt_mod_scmi_v2_1, */
	}, {
		.base = 0xc6480000 /* mdlc_top */,
		/* FIXME .mod_map = r8a78000_mdlc_top_mod_scmi_v2_1, */
	}, {
		.base = 0xde200000 /* mdlc_hscs */,
		/* FIXME .power_map = r8a78000_mdlc_hscs_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_hscs_mod_scmi_v2_1, */
	}, {
		.base = 0xc1990000 /* mdlc_imn */,
		/* FIXME .power_map = r8a78000_mdlc_imn_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_imn_mod_scmi_v2_1, */
	}, {
		.base = 0xc1d90000 /* mdlc_ims */,
		/* FIXME .power_map = r8a78000_mdlc_ims_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_ims_mod_scmi_v2_1, */
	}, {
		.base = 0xcb510000 /* mdlc_gpc */,
		/* FIXME .power_map = r8a78000_mdlc_gpc_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_gpc_mod_scmi_v2_1, */
	}, {
		.base = 0xcbe90000 /* mdlc_dsp */,
		/* FIXME .power_map = r8a78000_mdlc_dsp_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_dsp_mod_scmi_v2_1, */
	}, {
		.base = 0xe9980000 /* mdlc_mm */,
		/* FIXME .mod_map = r8a78000_mdlc_mm_mod_scmi_v2_1, */
	}, {
		.base = 0xd2c30000 /* mdlc_npu0 */,
		/* FIXME .power_map = r8a78000_mdlc_npu0_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_npu0_mod_scmi_v2_1, */
	}, {
		.base = 0xd6c30000 /* mdlc_npu1 */,
		/* FIXME .power_map = r8a78000_mdlc_npu1_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_npu1_mod_scmi_v2_1, */
	}, {
		.base = 0xca410000 /* mdlc_cmnn */,
		/* FIXME .power_map = r8a78000_mdlc_cmnn_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_cmnn_mod_scmi_v2_1, */
	}, {
		.base = 0xca510000 /* mdlc_cmns */,
		/* FIXME .power_map = r8a78000_mdlc_cmns_power_scmi_v2_1, */
		/* FIXME .mod_map = r8a78000_mdlc_cmns_mod_scmi_v2_1, */
	}, {
		.base = 0xc1330000 /* mdlc_scp */,
		/* FIXME .mod_map = r8a78000_mdlc_scp_mod_scmi_v2_1, */
	}, {
		.base = 0xc1338000 /* mdlc_aon */,
		/* FIXME .mod_map = r8a78000_mdlc_aon_mod_scmi_v2_1, */
	},
	{ 0 }
};

static const struct of_device_id r8a78000_mdlc_match[] = {
	{
		.compatible = "renesas,r8a78000-mdlc",
		.data = &r8a78000_mdlc_info,
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
