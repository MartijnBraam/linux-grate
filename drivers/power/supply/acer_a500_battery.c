// SPDX-License-Identifier: GPL-2.0+
/*
 * Battery driver for Acer Iconia Tab A500.
 *
 * Copyright 2020 GRATE-driver project.
 *
 * Based on downstream driver from Acer Inc.
 * Based on NVIDIA Gas Gauge driver for SBS Compliant Batteries.
 *
 * Copyright (c) 2010, NVIDIA Corporation.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <linux/mfd/acer-ec-a500.h>

#define BATTERY_SERIAL_LEN	22

enum {
	REG_CAPACITY,
	REG_VOLTAGE,
	REG_CURRENT,
	REG_DESIGN_CAPACITY,
	REG_TEMPERATURE,
	REG_SERIAL_NUMBER,
};

#define EC_DATA(_cmd, _delay, _psp) {		\
	.psp = POWER_SUPPLY_PROP_ ## _psp,	\
	.cmd = {				\
		.cmd = _cmd,			\
		.post_delay = _delay		\
	},					\
}

static const struct chip_data {
	enum power_supply_property psp;
	struct a500_ec_cmd cmd;
} ec_data[] = {
	[REG_CAPACITY]		= EC_DATA(0x00,  0, CAPACITY),
	[REG_VOLTAGE]		= EC_DATA(0x01,  0, VOLTAGE_NOW),
	[REG_CURRENT]		= EC_DATA(0x03, 10, CURRENT_NOW),
	[REG_DESIGN_CAPACITY]	= EC_DATA(0x08,  0, CHARGE_FULL_DESIGN),
	[REG_TEMPERATURE]	= EC_DATA(0x0a,  0, TEMP),
	[REG_SERIAL_NUMBER]	= EC_DATA(0x6a,  0, SERIAL_NUMBER),
};

static const enum power_supply_property a500_battery_properties[] = {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

struct a500_battery {
	struct delayed_work poll_work;
	struct power_supply *psy;
	struct a500_ec *ec_chip;
	unsigned int capacity;
	char serial[BATTERY_SERIAL_LEN + 1];
};

static int a500_battery_get_presence(struct a500_battery *bat,
				     union power_supply_propval *val)
{
	s32 ret;

	/*
	 * DESIGN_CAPACITY register always returns a non-zero value if
	 * battery is connected and zero if disconnected.
	 */
	ret = a500_ec_read(bat->ec_chip, &ec_data[REG_DESIGN_CAPACITY].cmd);
	if (ret <= 0)
		val->intval = 0;
	else
		val->intval = 1;

	return 0;
}

static bool a500_battery_update_capacity(struct a500_battery *bat)
{
	unsigned int capacity;
	s32 ret;

	ret = a500_ec_read(bat->ec_chip, &ec_data[REG_CAPACITY].cmd);
	if (ret < 0)
		return false;

	/* capacity can be >100% even if max value is 100% */
	capacity = min(ret, 100);

	if (bat->capacity != capacity) {
		bat->capacity = capacity;
		return true;
	}

	return false;
}

static void a500_battery_get_status(struct a500_battery *bat,
				    union power_supply_propval *val)
{
	if (bat->capacity < 100) {
		if (power_supply_am_i_supplied(bat->psy))
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
	} else {
		val->intval = POWER_SUPPLY_STATUS_FULL;
	}
}

static void a500_battery_unit_adjustment(struct device *dev,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	const unsigned int base_unit_conversion = 1000;
	const unsigned int temp_kelvin_to_celsius = 2731;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval *= base_unit_conversion;
		break;

	case POWER_SUPPLY_PROP_TEMP:
		val->intval -= temp_kelvin_to_celsius;
		break;

	default:
		dev_dbg(dev,
			"%s: no need for unit conversion %d\n", __func__, psp);
	}
}

static int a500_battery_get_serial_number(struct a500_battery *bat,
					  union power_supply_propval *val)
{
	unsigned int i;
	s32 ret = 0;

	if (bat->serial[0])
		goto done;

	a500_ec_lock(bat->ec_chip);
	for (i = 0; i < BATTERY_SERIAL_LEN / 2; i++) {
		ret = a500_ec_read_locked(bat->ec_chip,
					  &ec_data[REG_SERIAL_NUMBER].cmd);
		if (ret < 0) {
			bat->serial[0] = '\0';
			break;
		}

		bat->serial[i * 2 + 0] = (ret >> 0) & 0xff;
		bat->serial[i * 2 + 1] = (ret >> 8) & 0xff;
	}
	a500_ec_unlock(bat->ec_chip);
done:
	val->strval = bat->serial;

	return ret;
}

static int a500_battery_get_ec_data_index(struct device *dev,
					  enum power_supply_property psp)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ec_data); i++)
		if (psp == ec_data[i].psp)
			return i;

	dev_dbg(dev, "%s: invalid property %u\n", __func__, psp);

	return -EINVAL;
}

static int a500_battery_get_simple_property(struct a500_battery *bat,
					    union power_supply_propval *val,
					    unsigned int ec_idx)
{
	s32 ret;

	ret = a500_ec_read(bat->ec_chip, &ec_data[ec_idx].cmd);
	if (ret < 0)
		return ret;

	val->intval = (u16)ret;

	return 0;
}

static int a500_battery_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct a500_battery *bat = power_supply_get_drvdata(psy);
	struct device *dev = psy->dev.parent;
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		ret = a500_battery_get_serial_number(bat, val);
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		ret = a500_battery_get_presence(bat, val);
		break;

	case POWER_SUPPLY_PROP_STATUS:
		a500_battery_get_status(bat, val);
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		a500_battery_update_capacity(bat);
		val->intval = bat->capacity;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_TEMP:
		ret = a500_battery_get_ec_data_index(dev, psp);
		if (ret < 0)
			break;

		ret = a500_battery_get_simple_property(bat, val, ret);
		break;

	default:
		dev_err(dev, "%s: invalid property %u\n", __func__, psp);
		return -EINVAL;
	}

	if (!ret) {
		/* convert units to match requirements for power supply class */
		a500_battery_unit_adjustment(dev, psp, val);
	}

	dev_dbg(dev, "%s: property = %d, value = %x\n",
		__func__, psp, val->intval);

	/* return NODATA for properties if battery not presents */
	if (ret)
		return -ENODATA;

	return 0;
}

static void a500_battery_delayed_work(struct work_struct *work)
{
	struct a500_battery *bat;
	bool capacity_changed;

	bat = container_of(work, struct a500_battery, poll_work.work);
	capacity_changed = a500_battery_update_capacity(bat);

	if (capacity_changed)
		power_supply_changed(bat->psy);

	/* continuously send uevent notification */
	schedule_delayed_work(&bat->poll_work, 30 * HZ);
}

static const struct power_supply_desc a500_battery_desc = {
	.name = "ec-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = a500_battery_properties,
	.get_property = a500_battery_get_property,
	.num_properties = ARRAY_SIZE(a500_battery_properties),
	.external_power_changed = power_supply_changed,
};

static int a500_battery_probe(struct platform_device *pdev)
{
	struct power_supply_config psy_cfg = {};
	struct a500_battery *bat;
	int err;

	bat = devm_kzalloc(&pdev->dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	platform_set_drvdata(pdev, bat);

	psy_cfg.of_node = pdev->dev.parent->of_node;
	psy_cfg.drv_data = bat;

	bat->ec_chip = dev_get_drvdata(pdev->dev.parent);

	bat->psy = devm_power_supply_register_no_ws(&pdev->dev,
						    &a500_battery_desc,
						    &psy_cfg);
	err = PTR_ERR_OR_ZERO(bat->psy);
	if (err) {
		if (err == -EPROBE_DEFER)
			dev_dbg(&pdev->dev, "failed to register battery, deferring probe\n");
		else
			dev_err(&pdev->dev, "failed to register battery: %d\n",
				err);
		return err;
	}

	INIT_DELAYED_WORK(&bat->poll_work, a500_battery_delayed_work);
	schedule_delayed_work(&bat->poll_work, HZ);

	return 0;
}

static int a500_battery_remove(struct platform_device *pdev)
{
	struct a500_battery *bat = dev_get_drvdata(&pdev->dev);

	cancel_delayed_work_sync(&bat->poll_work);

	return 0;
}

static int __maybe_unused a500_battery_suspend(struct device *dev)
{
	struct a500_battery *bat = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&bat->poll_work);

	return 0;
}

static int __maybe_unused a500_battery_resume(struct device *dev)
{
	struct a500_battery *bat = dev_get_drvdata(dev);

	schedule_delayed_work(&bat->poll_work, HZ);

	return 0;
}

static SIMPLE_DEV_PM_OPS(a500_battery_pm_ops,
			 a500_battery_suspend, a500_battery_resume);

static struct platform_driver a500_battery_driver = {
	.driver = {
		.name = "acer-a500-iconia-battery",
		.pm = &a500_battery_pm_ops,
	},
	.probe = a500_battery_probe,
	.remove = a500_battery_remove,
};
module_platform_driver(a500_battery_driver);

MODULE_DESCRIPTION("Battery gauge driver for Acer Iconia Tab A500");
MODULE_AUTHOR("Dmitry Osipenko <digetx@gmail.com>");
MODULE_ALIAS("platform:acer-a500-iconia-battery");
MODULE_LICENSE("GPL v2");
