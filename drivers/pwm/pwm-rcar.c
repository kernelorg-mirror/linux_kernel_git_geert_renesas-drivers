// SPDX-License-Identifier: GPL-2.0
/*
 * R-Car PWM Timer driver
 *
 * Copyright (C) 2015 Renesas Electronics Corporation
 *
 * Limitations:
 * - The hardware cannot generate a 0% duty cycle.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/pwm.h>
#include <linux/slab.h>

#define RCAR_PWM_MAX_DIVISION	24
#define RCAR_PWM_MAX_CYCLE(rp)	GENMASK(rp->cycle_bits - 1, 0)

#define RCAR_PWMCR		0x00
#define  RCAR_PWMCR_CC0_MASK	GENMASK(19, 16)
#define  RCAR_PWMCR_CCMD	BIT(15)
#define  RCAR_PWMCR_SYNC	BIT(11)
#define  RCAR_PWMCR_SS0		BIT(4)
#define  RCAR_PWMCR_EN0		BIT(0)

#define RCAR_PWMCNT		0x04
#define  RCAR_PWMCNT_CYC0_MASK	GENMASK(31, 16)
#define  RCAR_PWMCNT_PH0_MASK	GENMASK(15, 0)

struct rcar_pwm_chip {
	void __iomem *base;
	struct clk *clk;
	unsigned int cycle_bits;
};

struct rcar_pwm_info {
	unsigned int cycle_bits;
};

static inline struct rcar_pwm_chip *to_rcar_pwm_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static void rcar_pwm_write(struct rcar_pwm_chip *rp, u32 data,
			   unsigned int offset)
{
	writel(data, rp->base + offset);
}

static u32 rcar_pwm_read(struct rcar_pwm_chip *rp, unsigned int offset)
{
	return readl(rp->base + offset);
}

static void rcar_pwm_update(struct rcar_pwm_chip *rp, u32 mask, u32 data,
			    unsigned int offset)
{
	u32 value;

	value = rcar_pwm_read(rp, offset);
	value &= ~mask;
	value |= data & mask;
	rcar_pwm_write(rp, value, offset);
}

static int rcar_pwm_get_clock_division(struct rcar_pwm_chip *rp, int period_ns)
{
	unsigned long clk_rate = clk_get_rate(rp->clk);
	u64 div, tmp;

	if (clk_rate == 0)
		return -EINVAL;

	div = mul_u32_u32(NSEC_PER_SEC, RCAR_PWM_MAX_CYCLE(rp));
	tmp = (u64)period_ns * clk_rate + div - 1;
	tmp = div64_u64(tmp, div);
	div = ilog2(tmp - 1) + 1;

	return (div <= RCAR_PWM_MAX_DIVISION) ? div : -ERANGE;
}

static void rcar_pwm_set_clock_control(struct rcar_pwm_chip *rp,
				       unsigned int div)
{
	u32 value;

	value = rcar_pwm_read(rp, RCAR_PWMCR);
	value &= ~(RCAR_PWMCR_CCMD | RCAR_PWMCR_CC0_MASK);

	if (div & 1)
		value |= RCAR_PWMCR_CCMD;

	div >>= 1;

	value |= FIELD_PREP(RCAR_PWMCR_CC0_MASK, div);
	rcar_pwm_write(rp, value, RCAR_PWMCR);
}

static int rcar_pwm_set_counter(struct rcar_pwm_chip *rp, int div, u64 duty_ns,
				u64 period_ns)
{
	unsigned long long tmp;
	unsigned long clk_rate = clk_get_rate(rp->clk);
	u32 cyc, ph;

	/* div <= 24 == RCAR_PWM_MAX_DIVISION, so the shift doesn't overflow. */
	tmp = mul_u64_u64_div_u64(period_ns, clk_rate, (u64)NSEC_PER_SEC << div);
	cyc = min(tmp, RCAR_PWM_MAX_CYCLE(rp));

	tmp = mul_u64_u64_div_u64(duty_ns, clk_rate, (u64)NSEC_PER_SEC << div);
	ph = min(tmp, RCAR_PWM_MAX_CYCLE(rp));

	/* Avoid prohibited setting */
	if (cyc == 0 || ph == 0)
		return -EINVAL;

	rcar_pwm_write(rp,
		       FIELD_PREP(RCAR_PWMCNT_CYC0_MASK, cyc) |
		       FIELD_PREP(RCAR_PWMCNT_PH0_MASK, ph),
		       RCAR_PWMCNT);

	return 0;
}

static int rcar_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	return pm_runtime_get_sync(pwmchip_parent(chip));
}

static void rcar_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	pm_runtime_put(pwmchip_parent(chip));
}

static int rcar_pwm_enable(struct rcar_pwm_chip *rp)
{
	u32 value, cyc, ph;

	/* Don't enable the PWM device if CYC0 or PH0 is 0 */
	value = rcar_pwm_read(rp, RCAR_PWMCNT);
	cyc = FIELD_GET(RCAR_PWMCNT_CYC0_MASK, value);
	ph = FIELD_GET(RCAR_PWMCNT_PH0_MASK, value);

	if ((cyc & RCAR_PWM_MAX_CYCLE(rp)) == 0 ||
	    (ph & RCAR_PWM_MAX_CYCLE(rp)) == 0)
		return -EINVAL;

	rcar_pwm_update(rp, RCAR_PWMCR_EN0, RCAR_PWMCR_EN0, RCAR_PWMCR);

	return 0;
}

static void rcar_pwm_disable(struct rcar_pwm_chip *rp)
{
	rcar_pwm_update(rp, RCAR_PWMCR_EN0, 0, RCAR_PWMCR);
}

static int rcar_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			  const struct pwm_state *state)
{
	struct rcar_pwm_chip *rp = to_rcar_pwm_chip(chip);
	int div, ret;

	/* This HW/driver only supports normal polarity */
	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled) {
		rcar_pwm_disable(rp);
		return 0;
	}

	div = rcar_pwm_get_clock_division(rp, state->period);
	if (div < 0)
		return div;

	rcar_pwm_update(rp, RCAR_PWMCR_SYNC, RCAR_PWMCR_SYNC, RCAR_PWMCR);

	ret = rcar_pwm_set_counter(rp, div, state->duty_cycle, state->period);
	if (!ret)
		rcar_pwm_set_clock_control(rp, div);

	/* The SYNC should be set to 0 even if rcar_pwm_set_counter failed */
	rcar_pwm_update(rp, RCAR_PWMCR_SYNC, 0, RCAR_PWMCR);

	if (!ret)
		ret = rcar_pwm_enable(rp);

	return ret;
}

static const struct pwm_ops rcar_pwm_ops = {
	.request = rcar_pwm_request,
	.free = rcar_pwm_free,
	.apply = rcar_pwm_apply,
};

static int rcar_pwm_probe(struct platform_device *pdev)
{
	const struct rcar_pwm_info *info;
	struct device *dev = &pdev->dev;
	struct rcar_pwm_chip *rcar_pwm;
	struct pwm_chip *chip;
	int ret;

	info = of_device_get_match_data(dev);
	if (!info)
		return -ENODEV;

	chip = devm_pwmchip_alloc(dev, 1, sizeof(*rcar_pwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	rcar_pwm = to_rcar_pwm_chip(chip);

	rcar_pwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rcar_pwm->base))
		return PTR_ERR(rcar_pwm->base);

	rcar_pwm->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(rcar_pwm->clk)) {
		dev_err(dev, "cannot get clock\n");
		return PTR_ERR(rcar_pwm->clk);
	}

	rcar_pwm->cycle_bits = info->cycle_bits;

	chip->ops = &rcar_pwm_ops;

	platform_set_drvdata(pdev, chip);

	pm_runtime_enable(dev);

	ret = pwmchip_add(chip);
	if (ret < 0) {
		dev_err(dev, "failed to register PWM chip: %d\n", ret);
		pm_runtime_disable(dev);
		return ret;
	}

	return 0;
}

static void rcar_pwm_remove(struct platform_device *pdev)
{
	struct pwm_chip *chip = platform_get_drvdata(pdev);

	pwmchip_remove(chip);

	pm_runtime_disable(&pdev->dev);
}

static const struct rcar_pwm_info rcar_pwm_info = {
	.cycle_bits = 10,
};

static const struct of_device_id rcar_pwm_of_table[] = {
	{ .compatible = "renesas,pwm-rcar", .data = &rcar_pwm_info },
	{ }
};
MODULE_DEVICE_TABLE(of, rcar_pwm_of_table);

static struct platform_driver rcar_pwm_driver = {
	.probe = rcar_pwm_probe,
	.remove = rcar_pwm_remove,
	.driver = {
		.name = "pwm-rcar",
		.of_match_table = rcar_pwm_of_table,
	}
};
module_platform_driver(rcar_pwm_driver);

MODULE_AUTHOR("Yoshihiro Shimoda <yoshihiro.shimoda.uh@renesas.com>");
MODULE_DESCRIPTION("Renesas PWM Timer Driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:pwm-rcar");
