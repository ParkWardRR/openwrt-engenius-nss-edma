// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm IPQ807x LED controller (LEDC).
 *
 * Derived from the QSDK driver drivers/leds/leds-ipq.c
 * (Copyright (c) 2015-2016, 2020 The Linux Foundation, ISC-licensed), with the
 * register model kept verbatim and the following changes for a modern kernel
 * and for OpenWrt:
 *
 *   - .remove returns void (kernel >= 6.11) and the driver uses devm for
 *     ioremap and LED registration, so the error paths go away;
 *   - LEDs register through led_classdev_register_ext() with led_init_data
 *     taken from each child node, so the DT's label/function/color decide the
 *     sysfs name. The vendor driver hardcoded "ipq::led%d" into a *stack*
 *     buffer and left cdev.name pointing at it after probe returned;
 *   - module_platform_driver() instead of module_platform_driver_probe(), so
 *     deferred probe works (pinctrl for the LEDC pins may not be ready yet);
 *   - the exported ipq_led_set_blink()/set_brightness() helpers are dropped;
 *     nothing in OpenWrt calls them.
 *
 * Hardware model, as used by the vendor firmware on the EnGenius ECW336:
 *   12 "CG" registers live at base + LEDC_BASE_REG_OFFSET, initialised
 *   wholesale from qti,tcsr_ledc_values. Brightness is one bit in CG6, at the
 *   bit position named by qti,ledc_blink_indices[i]; blink is a byte field in
 *   CG9 (enable | freq | duty).
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/leds.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>

#define LEDC_BASE_REG_OFFSET	0x20040
#define LEDC_REG_SIZE		4
#define LEDC_MAX_OFFSET		12
#define MAX_LEDS		13

/* First hardware blink source; sources below this are link/activity sources. */
#define LEDC_ST_BLINK_0		19

#define LEDC_CG6_OFFSET		6	/* on/off, one bit per LED index */
#define LEDC_CG9_OFFSET		9	/* blink config, one byte per source */

enum blink_freq_values {
	BLINK_2HZ, BLINK_4HZ, BLINK_8HZ, BLINK_16HZ, BLINK_32HZ, BLINK_64HZ
};

enum blink_duty_values {
	BLINK_DUTY_50, BLINK_DUTY_25, BLINK_DUTY_33, BLINK_DUTY_67,
	BLINK_DUTY_75
};

struct ipq_ledc;

struct ipq_led {
	struct led_classdev cdev;
	struct ipq_ledc *ledc;
	u32 hw_idx;		/* bit position in CG6 */
	int blink_src;		/* -1 if the LED has no hardware blink source */
};

struct ipq_ledc {
	void __iomem *base;
	struct device *dev;
	unsigned int num_leds;
	struct ipq_led leds[MAX_LEDS];
};

static void __iomem *ledc_reg(struct ipq_ledc *ledc, unsigned int idx)
{
	return ledc->base + LEDC_BASE_REG_OFFSET + LEDC_REG_SIZE * idx;
}

static int ipq_led_blink_set(struct led_classdev *cdev,
			     unsigned long *delay_on, unsigned long *delay_off)
{
	struct ipq_led *led = container_of(cdev, struct ipq_led, cdev);
	int blink_enable, blink_freq = 0, blink_duty = 0, blink_value;
	int total_on_off, duty_cycle, shift;
	u32 reg, mask;

	blink_enable = (*delay_on > 0);

	if (blink_enable) {
		total_on_off = *delay_on + *delay_off;

		/* Round to the nearest frequency the hardware can do. */
		if (total_on_off >= 500)
			blink_freq = BLINK_2HZ;
		else if (total_on_off >= 250)
			blink_freq = BLINK_4HZ;
		else if (total_on_off >= 125)
			blink_freq = BLINK_8HZ;
		else if (total_on_off >= 62)
			blink_freq = BLINK_16HZ;
		else if (total_on_off >= 31)
			blink_freq = BLINK_32HZ;
		else
			blink_freq = BLINK_64HZ;

		blink_freq <<= 1;

		duty_cycle = total_on_off ? (*delay_on * 100) / total_on_off : 50;
		if (duty_cycle >= 75)
			blink_duty = BLINK_DUTY_75;
		else if (duty_cycle >= 67)
			blink_duty = BLINK_DUTY_67;
		else if (duty_cycle >= 50)
			blink_duty = BLINK_DUTY_50;
		else if (duty_cycle >= 33)
			blink_duty = BLINK_DUTY_33;
		else
			blink_duty = BLINK_DUTY_25;

		blink_duty <<= 5;
	}

	blink_value = blink_enable | blink_freq | blink_duty;

	/*
	 * SoCs with a limited number of blink sources place the byte at the
	 * source's position; otherwise it sits at the LED's own index.
	 */
	if (led->blink_src >= LEDC_ST_BLINK_0)
		shift = 8 * (led->blink_src % LEDC_ST_BLINK_0);
	else
		shift = 8 * led->hw_idx;

	mask = ~(0xFF << shift);

	reg = readl(ledc_reg(led->ledc, LEDC_CG9_OFFSET));
	reg = (reg & mask) | (blink_value << shift);
	writel(reg, ledc_reg(led->ledc, LEDC_CG9_OFFSET));

	return 0;
}

static void ipq_led_brightness_set(struct led_classdev *cdev,
				   enum led_brightness brightness)
{
	struct ipq_led *led = container_of(cdev, struct ipq_led, cdev);
	u32 reg, mask;

	mask = BIT(led->hw_idx);

	reg = readl(ledc_reg(led->ledc, LEDC_CG6_OFFSET));
	if (brightness == LED_OFF)
		reg &= ~mask;
	else
		reg |= mask;
	writel(reg, ledc_reg(led->ledc, LEDC_CG6_OFFSET));
}

static int ipq_ledc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	u32 tcsr[LEDC_MAX_OFFSET];
	u32 hw_idx[MAX_LEDS];
	struct ipq_ledc *ledc;
	struct resource *res;
	u32 blink_cnt, i = 0;
	int ret, npairs;

	if (!np)
		return -ENODEV;

	ledc = devm_kzalloc(dev, sizeof(*ledc), GFP_KERNEL);
	if (!ledc)
		return -ENOMEM;
	ledc->dev = dev;

	/*
	 * Map without claiming the region. The LEDC registers live inside the
	 * TCSR block, which the hwspinlock driver already owns:
	 *
	 *   01905000-01924fff : 1905000.hwlock hwlock@1905000
	 *
	 * so request_mem_region() -- i.e. devm_platform_ioremap_resource*() --
	 * fails with -EBUSY. The vendor driver uses a bare ioremap() for the
	 * same reason.
	 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "ledc_base_addr");
	if (!res)
		return dev_err_probe(dev, -EINVAL,
				     "missing ledc_base_addr resource\n");

	ledc->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!ledc->base)
		return dev_err_probe(dev, -ENOMEM,
				     "cannot map ledc_base_addr\n");

	ret = of_property_read_u32_array(np, "qti,tcsr_ledc_values", tcsr,
					 LEDC_MAX_OFFSET);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing or short qti,tcsr_ledc_values\n");

	for (i = 0; i < LEDC_MAX_OFFSET; i++)
		writel(tcsr[i], ledc_reg(ledc, i));

	ret = of_property_read_u32(np, "qti,ledc_blink_indices_cnt",
				   &blink_cnt);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing qti,ledc_blink_indices_cnt\n");

	if (!blink_cnt || blink_cnt > MAX_LEDS)
		return dev_err_probe(dev, -EINVAL,
				     "bad qti,ledc_blink_indices_cnt %u\n",
				     blink_cnt);

	ret = of_property_read_u32_array(np, "qti,ledc_blink_indices", hw_idx,
					 blink_cnt);
	if (ret)
		return dev_err_probe(dev, ret,
				     "missing or short qti,ledc_blink_indices\n");

	/* Optional <led-index, blink-source> pairs. */
	npairs = of_property_count_u32_elems(np, "qti,ledc_blink_idx_src_pair");

	i = 0;
	for_each_available_child_of_node_scoped(np, child) {
		struct led_init_data init_data = {
			.fwnode = of_fwnode_handle(child),
		};
		struct ipq_led *led = &ledc->leds[i];

		if (i >= blink_cnt) {
			dev_warn(dev,
				 "more child nodes than blink indices (%u); ignoring the rest\n",
				 blink_cnt);
			break;
		}

		led->ledc = ledc;
		led->hw_idx = hw_idx[i];
		led->blink_src = -1;

		for (int p = 0; p + 1 < npairs; p += 2) {
			u32 idx, src;

			if (of_property_read_u32_index(np,
						       "qti,ledc_blink_idx_src_pair",
						       p, &idx) ||
			    of_property_read_u32_index(np,
						       "qti,ledc_blink_idx_src_pair",
						       p + 1, &src))
				break;
			if (idx == i)
				led->blink_src = src;
		}

		led->cdev.max_brightness = 1;
		led->cdev.brightness = LED_OFF;
		led->cdev.brightness_set = ipq_led_brightness_set;
		led->cdev.blink_set = ipq_led_blink_set;

		ret = devm_led_classdev_register_ext(dev, &led->cdev,
						     &init_data);
		if (ret)
			return dev_err_probe(dev, ret,
					     "cannot register LED %u\n", i);
		i++;
	}

	ledc->num_leds = i;
	if (!ledc->num_leds)
		return dev_err_probe(dev, -ENODEV, "no usable LED child nodes\n");

	platform_set_drvdata(pdev, ledc);
	dev_info(dev, "registered %u LEDs\n", ledc->num_leds);

	return 0;
}

static const struct of_device_id ipq_ledc_match[] = {
	{ .compatible = "qti,ledc" },
	{ }
};
MODULE_DEVICE_TABLE(of, ipq_ledc_match);

static struct platform_driver ipq_ledc_driver = {
	.probe = ipq_ledc_probe,
	.driver = {
		.name = "leds-ipq-ledc",
		.of_match_table = ipq_ledc_match,
	},
};
module_platform_driver(ipq_ledc_driver);

MODULE_DESCRIPTION("Qualcomm IPQ807x LED controller (LEDC)");
MODULE_LICENSE("GPL");
