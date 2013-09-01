/*
 * Copyright 2013 Stefan Kristiansson <stefan.kristiansson@saunalahti.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Generic driver for fixed rate clock
 */
#include <linux/platform_device.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/module.h>

static const struct of_device_id generic_fixed_clk_match[] = {
	{ .compatible = "fixed-clock",},
	{}
};

static int generic_fixed_clk_probe(struct platform_device *pdev)
{
	of_fixed_clk_setup(pdev->dev.of_node);

	return 0;
}

static int generic_fixed_clk_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);

	return 0;
}

static struct platform_driver generic_fixed_clk_driver = {
	.driver = {
		.name = "generic-fixed-clk",
		.owner = THIS_MODULE,
		.of_match_table = generic_fixed_clk_match,
	},
	.probe	= generic_fixed_clk_probe,
	.remove	= generic_fixed_clk_remove,
};

static int __init generic_fixed_clk_init(void)
{
	return platform_driver_register(&generic_fixed_clk_driver);
}
subsys_initcall(generic_fixed_clk_init);

static void __exit generic_fixed_exit(void)
{
	platform_driver_unregister(&generic_fixed_clk_driver);
}
module_exit(generic_fixed_exit);

MODULE_AUTHOR("Stefan Kristiansson <stefan.kristiansson@saunalahti.fi>");
MODULE_DESCRIPTION("Generic driver for fixed rate clock");
MODULE_LICENSE("GPL v2");
