// SPDX-License-Identifier: GPL-2.0
/*
 * SCMI Generic power domain support.
 *
 * Copyright (C) 2018-2021 ARM Ltd.
 */

#include <linux/clk.h>
#include <linux/clk/scmi.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_clock.h>
#include <linux/pm_domain.h>
#include <linux/scmi_protocol.h>

static const struct scmi_power_proto_ops *power_ops;

struct scmi_pm_domain {
	struct generic_pm_domain genpd;
	const struct scmi_protocol_handle *ph;
	struct device_node *clock_domain;
	const char *name;
	u32 domain;
};

#define to_scmi_pd(gpd) container_of(gpd, struct scmi_pm_domain, genpd)

static int scmi_pd_power(struct generic_pm_domain *domain, u32 state)
{
	struct scmi_pm_domain *pd = to_scmi_pd(domain);

	return power_ops->state_set(pd->ph, pd->domain, state);
}

static int scmi_pd_power_on(struct generic_pm_domain *domain)
{
	return scmi_pd_power(domain, SCMI_POWER_STATE_GENERIC_ON);
}

static int scmi_pd_power_off(struct generic_pm_domain *domain)
{
	return scmi_pd_power(domain, SCMI_POWER_STATE_GENERIC_OFF);
}

static int scmi_pd_attach_dev(struct generic_pm_domain *domain,
			      struct device *dev)
{
	struct scmi_pm_domain *pd = to_scmi_pd(domain);
	struct device_node *np = dev->of_node;
	struct of_phandle_args clkspec;
	bool once = true;
	struct clk *clk;
	int ret;

	for (int i = 0;
	     !of_parse_phandle_with_args(np, "clocks", "#clock-cells", i, &clkspec);
	     i++) {
		if (clkspec.np != pd->clock_domain || clkspec.args_count != 1) {
			of_node_put(clkspec.np);
			continue;
		}

		clk = of_clk_get_from_provider(&clkspec);
		of_node_put(clkspec.np);
		if (!clk)
			continue;

		if (IS_ERR(clk)) {
			ret = PTR_ERR(clk);
			clk = NULL;
			goto fail;
		}

		if (!scmi_clk_is_pm_clk(clk)) {
			clk_put(clk);
			continue;
		}

		if (once) {
			once = false;
			ret = pm_clk_create(dev);
			if (ret)
				goto fail;
		}

		ret = pm_clk_add_clk(dev, clk);
		if (ret)
			goto fail;
	}

	return 0;

fail:
	pm_clk_destroy(dev);
	clk_put(clk);
	return ret;
}

static void scmi_pd_detach_dev(struct generic_pm_domain *domain,
			       struct device *dev)
{
	if (!pm_clk_no_clocks(dev))
		pm_clk_destroy(dev);
}

static int scmi_pm_domain_probe(struct scmi_device *sdev)
{
	int num_domains, i, ret;
	struct device *dev = &sdev->dev;
	struct device_node *np = dev->of_node;
	struct scmi_pm_domain *scmi_pd;
	struct genpd_onecell_data *scmi_pd_data;
	struct generic_pm_domain **domains;
	const struct scmi_handle *handle = sdev->handle;
	struct device_node *clock_domain;
	struct scmi_protocol_handle *ph;

	if (!handle)
		return -ENODEV;

	power_ops = handle->devm_protocol_get(sdev, SCMI_PROTOCOL_POWER, &ph);
	if (IS_ERR(power_ops))
		return PTR_ERR(power_ops);

	num_domains = power_ops->num_domains_get(ph);
	if (num_domains < 0) {
		dev_err(dev, "number of domains not found\n");
		return num_domains;
	}

	scmi_pd = devm_kcalloc(dev, num_domains, sizeof(*scmi_pd), GFP_KERNEL);
	if (!scmi_pd)
		return -ENOMEM;

	scmi_pd_data = devm_kzalloc(dev, sizeof(*scmi_pd_data), GFP_KERNEL);
	if (!scmi_pd_data)
		return -ENOMEM;

	domains = devm_kcalloc(dev, num_domains, sizeof(*domains), GFP_KERNEL);
	if (!domains)
		return -ENOMEM;

	clock_domain = of_parse_phandle(np, "arm,clock-domain", 0);

	for (i = 0; i < num_domains; i++, scmi_pd++) {
		const struct scmi_power_domain_info *info;
		u32 state;

		info = power_ops->info_get(ph, i);
		if (!info) {
			dev_warn(dev, "failed to get info for domain %d\n", i);
			continue;
		}

		if (power_ops->state_get(ph, i, &state)) {
			dev_warn(dev, "failed to get state for domain %d\n", i);
			continue;
		}

		/*
		 * Register the explicit power on request to the firmware so
		 * that it is tracked as used by OSPM agent and not
		 * accidentally turned off with OSPM's knowledge
		 */
		if (state == SCMI_POWER_STATE_GENERIC_ON)
			power_ops->state_set(ph, i, state);

		scmi_pd->domain = i;
		scmi_pd->ph = ph;
		scmi_pd->name = info->name;
		scmi_pd->genpd.name = scmi_pd->name;
		scmi_pd->genpd.power_off = scmi_pd_power_off;
		scmi_pd->genpd.power_on = scmi_pd_power_on;
		scmi_pd->genpd.flags = GENPD_FLAG_ACTIVE_WAKEUP |
				       info->genpd_flags;
		if (clock_domain) {
			scmi_pd->clock_domain = of_node_get(clock_domain);
			scmi_pd->genpd.attach_dev = scmi_pd_attach_dev;
			scmi_pd->genpd.detach_dev = scmi_pd_detach_dev;
			scmi_pd->genpd.flags |= GENPD_FLAG_PM_CLK;
		}

		pm_genpd_init(&scmi_pd->genpd, NULL,
			      state == SCMI_POWER_STATE_GENERIC_OFF);

		domains[i] = &scmi_pd->genpd;
	}

	of_node_put(clock_domain);

	scmi_pd_data->domains = domains;
	scmi_pd_data->num_domains = num_domains;

	ret = of_genpd_add_provider_onecell(np, scmi_pd_data);
	if (ret)
		goto err_rm_genpds;

	dev_set_drvdata(dev, scmi_pd_data);

	/*
	 * Parse (optional) power-domains-child-ids property to establish
	 * parent-child relationships.
	*/
	ret = of_genpd_add_child_ids(np, scmi_pd_data);
	if (ret < 0)
		dev_err(dev, "Failed to add child domain hierarchy: %d\n", ret);

	dev_info(dev, "Initialized %d power domains", num_domains);

	return 0;
err_rm_genpds:
	for (i = num_domains - 1; i >= 0; i--) {
		pm_genpd_remove(domains[i]);
		of_node_put(to_scmi_pd(domains[i])->clock_domain);
	}

	return ret;
}

static void scmi_pm_domain_remove(struct scmi_device *sdev)
{
	int i;
	struct genpd_onecell_data *scmi_pd_data;
	struct device *dev = &sdev->dev;
	struct device_node *np = dev->of_node;

	scmi_pd_data = dev_get_drvdata(dev);

	/* Remove any parent-child relationships established at probe time */
	of_genpd_remove_child_ids(np, scmi_pd_data);

	of_genpd_del_provider(np);

	for (i = 0; i < scmi_pd_data->num_domains; i++) {
		if (!scmi_pd_data->domains[i])
			continue;
		pm_genpd_remove(scmi_pd_data->domains[i]);
		of_node_put(to_scmi_pd(scmi_pd_data->domains[i])->clock_domain);
	}
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_POWER, "genpd" },
	{ },
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver scmi_power_domain_driver = {
	.name = "scmi-power-domain",
	.probe = scmi_pm_domain_probe,
	.remove = scmi_pm_domain_remove,
	.id_table = scmi_id_table,
};
module_scmi_driver(scmi_power_domain_driver);

MODULE_AUTHOR("Sudeep Holla <sudeep.holla@arm.com>");
MODULE_DESCRIPTION("ARM SCMI power domain driver");
MODULE_LICENSE("GPL v2");
