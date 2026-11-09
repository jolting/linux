// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (C) 2010, Paul Cercueil <paul@crapouillou.net>
 *  JZ4740 Watchdog driver
 */

#include <linux/mfd/ingenic-tcu.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/watchdog.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define DEFAULT_HEARTBEAT 5
#define MAX_HEARTBEAT     2048

/*
 * The X2600's watchdog has the TCU's watchdog registers and its stop
 * bit, in a block of its own with no TCU clock driver to set up its
 * input: the driver counts the RTC clock divided by 1024. Its counter
 * ignores writes; TCSR's CLRZ bit clears it.
 */
#define X2600_WDT_TCSR_RTC_EN	BIT(1)
#define X2600_WDT_TCSR_CLRZ	BIT(10)
#define X2600_WDT_PRESCALE	5	/* 4^5 = 1024 */
#define X2600_WDT_STOP		BIT(16)	/* in TSSR, TSCR */

struct jz4740_wdt_soc_info {
	bool own_regs;
	bool clrz;
};

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static unsigned int heartbeat = DEFAULT_HEARTBEAT;
module_param(heartbeat, uint, 0);
MODULE_PARM_DESC(heartbeat,
		"Watchdog heartbeat period in seconds from 1 to "
		__MODULE_STRING(MAX_HEARTBEAT) ", default "
		__MODULE_STRING(DEFAULT_HEARTBEAT));

struct jz4740_wdt_drvdata {
	struct watchdog_device wdt;
	const struct jz4740_wdt_soc_info *soc_info;
	struct regmap *map;
	struct clk *clk;
	unsigned long clk_rate;
};

static void jz4740_wdt_clear(struct jz4740_wdt_drvdata *drvdata)
{
	if (drvdata->soc_info && drvdata->soc_info->clrz)
		regmap_set_bits(drvdata->map, TCU_REG_WDT_TCSR, X2600_WDT_TCSR_CLRZ);
	else
		regmap_write(drvdata->map, TCU_REG_WDT_TCNT, 0);
}

static int jz4740_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct jz4740_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);

	jz4740_wdt_clear(drvdata);

	return 0;
}

static int jz4740_wdt_set_timeout(struct watchdog_device *wdt_dev,
				    unsigned int new_timeout)
{
	struct jz4740_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);
	u16 timeout_value = (u16)(drvdata->clk_rate * new_timeout);
	unsigned int tcer;

	regmap_read(drvdata->map, TCU_REG_WDT_TCER, &tcer);
	regmap_write(drvdata->map, TCU_REG_WDT_TCER, 0);

	regmap_write(drvdata->map, TCU_REG_WDT_TDR, timeout_value);
	jz4740_wdt_clear(drvdata);

	if (tcer & TCU_WDT_TCER_TCEN)
		regmap_write(drvdata->map, TCU_REG_WDT_TCER, TCU_WDT_TCER_TCEN);

	wdt_dev->timeout = new_timeout;
	return 0;
}

static int jz4740_wdt_start(struct watchdog_device *wdt_dev)
{
	struct jz4740_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);
	unsigned int tcer;
	int ret;

	ret = clk_prepare_enable(drvdata->clk);
	if (ret)
		return ret;

	regmap_read(drvdata->map, TCU_REG_WDT_TCER, &tcer);

	jz4740_wdt_set_timeout(wdt_dev, wdt_dev->timeout);

	/* Start watchdog if it wasn't started already */
	if (!(tcer & TCU_WDT_TCER_TCEN))
		regmap_write(drvdata->map, TCU_REG_WDT_TCER, TCU_WDT_TCER_TCEN);

	return 0;
}

static int jz4740_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct jz4740_wdt_drvdata *drvdata = watchdog_get_drvdata(wdt_dev);

	regmap_write(drvdata->map, TCU_REG_WDT_TCER, 0);
	clk_disable_unprepare(drvdata->clk);

	return 0;
}

static int jz4740_wdt_restart(struct watchdog_device *wdt_dev,
			      unsigned long action, void *data)
{
	wdt_dev->timeout = 0;
	jz4740_wdt_start(wdt_dev);
	return 0;
}

static const struct watchdog_info jz4740_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "jz4740 Watchdog",
};

static const struct watchdog_ops jz4740_wdt_ops = {
	.owner = THIS_MODULE,
	.start = jz4740_wdt_start,
	.stop = jz4740_wdt_stop,
	.ping = jz4740_wdt_ping,
	.set_timeout = jz4740_wdt_set_timeout,
	.restart = jz4740_wdt_restart,
};

static const struct jz4740_wdt_soc_info x2600_wdt_soc_info = {
	.own_regs = true,
	.clrz = true,
};

#ifdef CONFIG_OF
static const struct of_device_id jz4740_wdt_of_matches[] = {
	{ .compatible = "ingenic,jz4740-watchdog", },
	{ .compatible = "ingenic,jz4780-watchdog", },
	{ .compatible = "ingenic,x2600-watchdog", .data = &x2600_wdt_soc_info },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, jz4740_wdt_of_matches);
#endif

static const struct regmap_config x2600_wdt_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = TCU_REG_TSCR,
};

static int x2600_wdt_init(struct platform_device *pdev,
			  struct jz4740_wdt_drvdata *drvdata)
{
	struct device *dev = &pdev->dev;
	struct clk *bus_clk;
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	drvdata->map = devm_regmap_init_mmio(dev, base, &x2600_wdt_regmap_config);
	if (IS_ERR(drvdata->map))
		return PTR_ERR(drvdata->map);

	bus_clk = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(bus_clk))
		return dev_err_probe(dev, PTR_ERR(bus_clk), "cannot get bus clock\n");

	drvdata->clk_rate = clk_get_rate(drvdata->clk) >> (2 * X2600_WDT_PRESCALE);
	if (!drvdata->clk_rate)
		return dev_err_probe(dev, -EINVAL, "WDT clock too slow\n");

	regmap_write(drvdata->map, TCU_REG_WDT_TCSR,
		     X2600_WDT_TCSR_RTC_EN |
		     (X2600_WDT_PRESCALE << TCU_TCSR_PRESCALE_LSB));
	regmap_write(drvdata->map, TCU_REG_TSCR, X2600_WDT_STOP);

	return 0;
}

static int jz4740_wdt_probe(struct platform_device *pdev)
{
	const struct jz4740_wdt_soc_info *soc_info;
	struct device *dev = &pdev->dev;
	struct jz4740_wdt_drvdata *drvdata;
	struct watchdog_device *jz4740_wdt;
	long rate;
	int ret;

	drvdata = devm_kzalloc(dev, sizeof(struct jz4740_wdt_drvdata),
			       GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->clk = devm_clk_get(&pdev->dev, "wdt");
	if (IS_ERR(drvdata->clk)) {
		dev_err(&pdev->dev, "cannot find WDT clock\n");
		return PTR_ERR(drvdata->clk);
	}

	soc_info = device_get_match_data(dev);
	drvdata->soc_info = soc_info;
	if (soc_info && soc_info->own_regs) {
		ret = x2600_wdt_init(pdev, drvdata);
		if (ret)
			return ret;
	} else {
		/* Set smallest clock possible */
		rate = clk_round_rate(drvdata->clk, 1);
		if (rate < 0)
			return rate;

		ret = clk_set_rate(drvdata->clk, rate);
		if (ret)
			return ret;

		drvdata->clk_rate = rate;

		drvdata->map = device_node_to_regmap(dev->parent->of_node);
		if (IS_ERR(drvdata->map)) {
			dev_err(dev, "regmap not found\n");
			return PTR_ERR(drvdata->map);
		}
	}

	jz4740_wdt = &drvdata->wdt;
	jz4740_wdt->info = &jz4740_wdt_info;
	jz4740_wdt->ops = &jz4740_wdt_ops;
	jz4740_wdt->min_timeout = 1;
	jz4740_wdt->max_timeout = 0xffff / drvdata->clk_rate;
	jz4740_wdt->timeout = clamp(heartbeat,
				    jz4740_wdt->min_timeout,
				    jz4740_wdt->max_timeout);
	jz4740_wdt->parent = dev;
	watchdog_set_nowayout(jz4740_wdt, nowayout);
	watchdog_set_drvdata(jz4740_wdt, drvdata);

	return devm_watchdog_register_device(dev, &drvdata->wdt);
}

static struct platform_driver jz4740_wdt_driver = {
	.probe = jz4740_wdt_probe,
	.driver = {
		.name = "jz4740-wdt",
		.of_match_table = of_match_ptr(jz4740_wdt_of_matches),
	},
};

module_platform_driver(jz4740_wdt_driver);

MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_DESCRIPTION("jz4740 Watchdog Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:jz4740-wdt");
