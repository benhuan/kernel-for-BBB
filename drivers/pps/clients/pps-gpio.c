/*
 * pps-gpio.c -- PPS client driver using GPIO
 *
 *
 * Copyright (C) 2010 Ricardo Martins <rasm@fe.up.pt>
 * Copyright (C) 2011 James Nuss <jamesnuss@nanometrics.ca>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define PPS_GPIO_NAME "pps-gpio"
#define pr_fmt(fmt) PPS_GPIO_NAME ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/pps_kernel.h>
#include <linux/pps-gpio.h>
#include <linux/gpio.h>
#include <linux/list.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

/* Info for each registered platform device */
struct pps_gpio_device_data {
	int irq;			/* IRQ used as PPS source */
	struct pps_device *pps;		/* PPS source device */
	struct pps_source_info info;	/* PPS source information */
	const struct pps_gpio_platform_data *pdata;
};

/*
 * Report the PPS event
 */

static irqreturn_t pps_gpio_irq_handler(int irq, void *data)
{
	const struct pps_gpio_device_data *info;
	struct pps_event_time ts;
	int rising_edge;

	/* Get the time stamp first */
	pps_get_ts(&ts);

	info = data;

	rising_edge = gpio_get_value(info->pdata->gpio_pin);
	if ((rising_edge && !info->pdata->assert_falling_edge) ||
			(!rising_edge && info->pdata->assert_falling_edge))
		pps_event(info->pps, &ts, PPS_CAPTUREASSERT, NULL);
	else if (info->pdata->capture_clear &&
			((rising_edge && info->pdata->assert_falling_edge) ||
			 (!rising_edge && !info->pdata->assert_falling_edge)))
		pps_event(info->pps, &ts, PPS_CAPTURECLEAR, NULL);

	return IRQ_HANDLED;
}

static int pps_gpio_setup(struct platform_device *pdev)
{
	int ret;
	const struct pps_gpio_platform_data *pdata = pdev->dev.platform_data;

	ret = gpio_request(pdata->gpio_pin, pdata->gpio_label);
	if (ret) {
		pr_warning("failed to request GPIO %u\n", pdata->gpio_pin);
		return -EINVAL;
	}

	ret = gpio_direction_input(pdata->gpio_pin);
	if (ret) {
		pr_warning("failed to set pin direction\n");
		gpio_free(pdata->gpio_pin);
		return -EINVAL;
	}

	return 0;
}

static unsigned long
get_irqf_trigger_flags(const struct pps_gpio_platform_data *pdata)
{
	unsigned long flags = pdata->assert_falling_edge ?
		IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;

	if (pdata->capture_clear) {
		flags |= ((flags & IRQF_TRIGGER_RISING) ?
				IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING);
	}

	return flags;
}

#ifdef CONFIG_OF
static const struct of_device_id pps_gpio_dt_ids[] = {
	{ .compatible = "pps-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pps_gpio_dt_ids);

static struct pps_gpio_platform_data *
of_get_pps_gpio_pdata(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct pps_gpio_platform_data *pdata;
	int ret;

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	ret = of_get_gpio(np, 0);
	if (ret < 0) {
		pr_err("failed to get GPIO from device tree\n");
		return NULL;
	}

	pdata->gpio_pin = ret;
	pdata->gpio_label = PPS_GPIO_NAME;

	if (of_get_property(np, "assert-falling-edge", NULL))
		pdata->assert_falling_edge = true;

	return pdata;
}
#else
static struct pps_gpio_platform_data *
of_get_pps_gpio_pdata(struct platform_device *pdev)
{
	return NULL;
}
#endif

static int pps_gpio_probe(struct platform_device *pdev)
{
	struct pps_gpio_device_data *data;
	int irq;
	int ret;
	int pps_default_params;
	struct pinctrl *pinctrl;
	struct pps_gpio_platform_data *pdata;
	const struct of_device_id *match;

	match = of_match_device(pps_gpio_dt_ids, &pdev->dev);
	if (match)
		pdev->dev.platform_data = of_get_pps_gpio_pdata(pdev);
	pdata = pdev->dev.platform_data;

	if (!pdata)
		return -ENODEV;

	/* PINCTL setup */
	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl))
		pr_warn("pins are not configured from the driver\n");

	/* GPIO setup */
	ret = pps_gpio_setup(pdev);
	if (ret)
		return -EINVAL;

	/* IRQ setup */
	irq = gpio_to_irq(pdata->gpio_pin);
	if (irq < 0) {
		pr_err("failed to map GPIO to IRQ: %d\n", irq);
		return -EINVAL;
	}

	/* allocate space for device info */
	data = kzalloc(sizeof(struct pps_gpio_device_data), GFP_KERNEL);
	if (data == NULL) {
		return -ENOMEM;
	}

	/* initialize PPS specific parts of the bookkeeping data structure. */
	data->info.mode = PPS_CAPTUREASSERT | PPS_OFFSETASSERT |
		PPS_ECHOASSERT | PPS_CANWAIT | PPS_TSFMT_TSPEC;
	if (pdata->capture_clear)
		data->info.mode |= PPS_CAPTURECLEAR | PPS_OFFSETCLEAR |
			PPS_ECHOCLEAR;
	data->info.owner = THIS_MODULE;
	snprintf(data->info.name, PPS_MAX_NAME_LEN - 1, "%s.%d",
		 pdev->name, pdev->id);

	/* register PPS source */
	pps_default_params = PPS_CAPTUREASSERT | PPS_OFFSETASSERT;
	if (pdata->capture_clear)
		pps_default_params |= PPS_CAPTURECLEAR | PPS_OFFSETCLEAR;
	data->pps = pps_register_source(&data->info, pps_default_params);
	if (data->pps == NULL) {
		kfree(data);
		pr_err("failed to register IRQ %d as PPS source\n", irq);
		return -EINVAL;
	}

	data->irq = irq;
	data->pdata = pdata;

	/* register IRQ interrupt handler */
	ret = devm_request_irq(&pdev->dev, irq, pps_gpio_irq_handler,
			get_irqf_trigger_flags(pdata), data->info.name, data);
	if (ret) {
		pps_unregister_source(data->pps);
		kfree(data);
		pr_err("failed to acquire IRQ %d\n", irq);
		return -EINVAL;
	}

	platform_set_drvdata(pdev, data);
	dev_info(data->pps->dev, "Registered IRQ %d as PPS source\n", irq);

	return 0;
}

static int pps_gpio_remove(struct platform_device *pdev)
{
	struct pps_gpio_device_data *data = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	pps_unregister_source(data->pps);
	pr_info("removed IRQ %d as PPS source\n", data->irq);
	kfree(data);
	return 0;
}

static struct platform_driver pps_gpio_driver = {
	.probe		= pps_gpio_probe,
	.remove		= pps_gpio_remove,
	.driver		= {
		.name	= PPS_GPIO_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= of_match_ptr(pps_gpio_dt_ids),
	},
};

module_platform_driver(pps_gpio_driver);
MODULE_AUTHOR("Ricardo Martins <rasm@fe.up.pt>");
MODULE_AUTHOR("James Nuss <jamesnuss@nanometrics.ca>");
MODULE_DESCRIPTION("Use GPIO pin as PPS source");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
