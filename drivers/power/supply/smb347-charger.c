// SPDX-License-Identifier: GPL-2.0-only
/*
 * Summit Microelectronics SMB347 Battery Charger Driver
 *
 * Copyright (C) 2011, Intel Corporation
 *
 * Authors: Bruce E. Robertson <bruce.e.robertson@intel.com>
 *          Mika Westerberg <mika.westerberg@linux.intel.com>
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/power_supply.h>
#include <linux/power/smb347-charger.h>
#include <linux/regmap.h>

/*
 * Configuration registers. These are mirrored to volatile RAM and can be
 * written once %CMD_A_ALLOW_WRITE is set in %CMD_A register. They will be
 * reloaded from non-volatile registers after POR.
 */
#define CFG_CHARGE_CURRENT			0x00
#define CFG_CHARGE_CURRENT_FCC_MASK		0xe0
#define CFG_CHARGE_CURRENT_FCC_SHIFT		5
#define CFG_CHARGE_CURRENT_PCC_MASK		0x18
#define CFG_CHARGE_CURRENT_PCC_SHIFT		3
#define CFG_CHARGE_CURRENT_TC_MASK		0x07
#define CFG_CURRENT_LIMIT			0x01
#define CFG_CURRENT_LIMIT_DC_MASK		0xf0
#define CFG_CURRENT_LIMIT_DC_SHIFT		4
#define CFG_CURRENT_LIMIT_USB_MASK		0x0f
#define CFG_FLOAT_VOLTAGE			0x03
#define CFG_FLOAT_VOLTAGE_FLOAT_MASK		0x3f
#define CFG_FLOAT_VOLTAGE_THRESHOLD_MASK	0xc0
#define CFG_FLOAT_VOLTAGE_THRESHOLD_SHIFT	6
#define CFG_STAT				0x05
#define CFG_STAT_DISABLED			BIT(5)
#define CFG_STAT_ACTIVE_HIGH			BIT(7)
#define CFG_PIN					0x06
#define CFG_PIN_EN_CTRL_MASK			0x60
#define CFG_PIN_EN_CTRL_ACTIVE_HIGH		0x40
#define CFG_PIN_EN_CTRL_ACTIVE_LOW		0x60
#define CFG_PIN_EN_APSD_IRQ			BIT(1)
#define CFG_PIN_EN_CHARGER_ERROR		BIT(2)
#define CFG_THERM				0x07
#define CFG_THERM_SOFT_HOT_COMPENSATION_MASK	0x03
#define CFG_THERM_SOFT_HOT_COMPENSATION_SHIFT	0
#define CFG_THERM_SOFT_COLD_COMPENSATION_MASK	0x0c
#define CFG_THERM_SOFT_COLD_COMPENSATION_SHIFT	2
#define CFG_THERM_MONITOR_DISABLED		BIT(4)
#define CFG_SYSOK				0x08
#define CFG_SYSOK_SUSPEND_HARD_LIMIT_DISABLED	BIT(2)
#define CFG_OTHER				0x09
#define CFG_OTHER_RID_MASK			0xc0
#define CFG_OTHER_RID_ENABLED_AUTO_OTG		0xc0
#define CFG_OTG					0x0a
#define CFG_OTG_TEMP_THRESHOLD_MASK		0x30
#define CFG_OTG_TEMP_THRESHOLD_SHIFT		4
#define CFG_OTG_CC_COMPENSATION_MASK		0xc0
#define CFG_OTG_CC_COMPENSATION_SHIFT		6
#define CFG_TEMP_LIMIT				0x0b
#define CFG_TEMP_LIMIT_SOFT_HOT_MASK		0x03
#define CFG_TEMP_LIMIT_SOFT_HOT_SHIFT		0
#define CFG_TEMP_LIMIT_SOFT_COLD_MASK		0x0c
#define CFG_TEMP_LIMIT_SOFT_COLD_SHIFT		2
#define CFG_TEMP_LIMIT_HARD_HOT_MASK		0x30
#define CFG_TEMP_LIMIT_HARD_HOT_SHIFT		4
#define CFG_TEMP_LIMIT_HARD_COLD_MASK		0xc0
#define CFG_TEMP_LIMIT_HARD_COLD_SHIFT		6
#define CFG_FAULT_IRQ				0x0c
#define CFG_FAULT_IRQ_DCIN_UV			BIT(2)
#define CFG_STATUS_IRQ				0x0d
#define CFG_STATUS_IRQ_TERMINATION_OR_TAPER	BIT(4)
#define CFG_STATUS_IRQ_CHARGE_TIMEOUT		BIT(7)
#define CFG_ADDRESS				0x0e

/* Command registers */
#define CMD_A					0x30
#define CMD_A_CHG_ENABLED			BIT(1)
#define CMD_A_SUSPEND_ENABLED			BIT(2)
#define CMD_A_ALLOW_WRITE			BIT(7)
#define CMD_B					0x31
#define CMD_C					0x33

/* Interrupt Status registers */
#define IRQSTAT_A				0x35
#define IRQSTAT_C				0x37
#define IRQSTAT_C_TERMINATION_STAT		BIT(0)
#define IRQSTAT_C_TERMINATION_IRQ		BIT(1)
#define IRQSTAT_C_TAPER_IRQ			BIT(3)
#define IRQSTAT_D				0x38
#define IRQSTAT_D_CHARGE_TIMEOUT_STAT		BIT(2)
#define IRQSTAT_D_CHARGE_TIMEOUT_IRQ		BIT(3)
#define IRQSTAT_E				0x39
#define IRQSTAT_E_USBIN_UV_STAT			BIT(0)
#define IRQSTAT_E_USBIN_UV_IRQ			BIT(1)
#define IRQSTAT_E_DCIN_UV_STAT			BIT(4)
#define IRQSTAT_E_DCIN_UV_IRQ			BIT(5)
#define IRQSTAT_F				0x3a

/* Status registers */
#define STAT_A					0x3b
#define STAT_A_FLOAT_VOLTAGE_MASK		0x3f
#define STAT_B					0x3c
#define STAT_C					0x3d
#define STAT_C_CHG_ENABLED			BIT(0)
#define STAT_C_HOLDOFF_STAT			BIT(3)
#define STAT_C_CHG_MASK				0x06
#define STAT_C_CHG_SHIFT			1
#define STAT_C_CHG_TERM				BIT(5)
#define STAT_C_CHARGER_ERROR			BIT(6)
#define STAT_E					0x3f

#define SMB347_MAX_REGISTER			0x3f

/**
 * struct smb347_charger - smb347 charger instance
 * @dev: pointer to device
 * @regmap: pointer to driver regmap
 * @mains: power_supply instance for AC/DC power
 * @usb: power_supply instance for USB power
 * @id: SMB charger ID
 * @mains_online: is AC/DC input connected
 * @usb_online: is USB input connected
 * @charging_enabled: is charging enabled
 * @pdata: pointer to platform data
 */
struct smb347_charger {
	struct device		*dev;
	struct regmap		*regmap;
	struct power_supply	*mains;
	struct power_supply	*usb;
	unsigned int		id;
	bool			mains_online;
	bool			usb_online;
	bool			charging_enabled;
	const struct smb347_charger_platform_data *pdata;
};

enum smb_charger_chipid {
	SMB345,
	SMB347,
	SMB358,
	NUM_CHIP_TYPES,
};

/* Fast charge current in uA */
static const unsigned int fcc_tbl[NUM_CHIP_TYPES][8] = {
	[SMB345] = {  200000,  450000,  600000,  900000,
		     1300000, 1500000, 1800000, 2000000 },
	[SMB347] = {  700000,  900000, 1200000, 1500000,
		     1800000, 2000000, 2200000, 2500000 },
	[SMB358] = {  200000,  450000,  600000,  900000,
		     1300000, 1500000, 1800000, 2000000 },
};
/* Pre-charge current in uA */
static const unsigned int pcc_tbl[NUM_CHIP_TYPES][4] = {
	[SMB345] = { 150000, 250000, 350000, 450000 },
	[SMB347] = { 100000, 150000, 200000, 250000 },
	[SMB358] = { 150000, 250000, 350000, 450000 },
};

/* Termination current in uA */
static const unsigned int tc_tbl[NUM_CHIP_TYPES][8] = {
	[SMB345] = {  30000,  40000,  60000,  80000,
		     100000, 125000, 150000, 200000 },
	[SMB347] = {  37500,  50000, 100000, 150000,
		     200000, 250000, 500000, 600000 },
	[SMB358] = {  30000,  40000,  60000,  80000,
		     100000, 125000, 150000, 200000 },
};

/* Input current limit in uA */
static const unsigned int icl_tbl[NUM_CHIP_TYPES][10] = {
	[SMB345] = {  300000,  500000,  700000, 1000000, 1500000,
		     1800000, 2000000, 2000000, 2000000, 2000000 },
	[SMB347] = {  300000,  500000,  700000,  900000, 1200000,
		     1500000, 1800000, 2000000, 2200000, 2500000 },
	[SMB358] = {  300000,  500000,  700000, 1000000, 1500000,
		     1800000, 2000000, 2000000, 2000000, 2000000 },
};

/* Charge current compensation in uA */
static const unsigned int ccc_tbl[NUM_CHIP_TYPES][4] = {
	[SMB345] = {  200000,  450000,  600000,  900000 },
	[SMB347] = {  250000,  700000,  900000, 1200000 },
	[SMB358] = {  200000,  450000,  600000,  900000 },
};

/* Convert register value to current using lookup table */
static int hw_to_current(const unsigned int *tbl, size_t size, unsigned int val)
{
	if (val >= size)
		return -EINVAL;
	return tbl[val];
}

/* Convert current to register value using lookup table */
static int current_to_hw(const unsigned int *tbl, size_t size, unsigned int val)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (val < tbl[i])
			break;
	return i > 0 ? i - 1 : -EINVAL;
}

/**
 * smb347_update_ps_status - refreshes the power source status
 * @smb: pointer to smb347 charger instance
 *
 * Function checks whether any power source is connected to the charger and
 * updates internal state accordingly. If there is a change to previous state
 * function returns %1, otherwise %0 and negative errno in case of errror.
 */
static int smb347_update_ps_status(struct smb347_charger *smb)
{
	bool usb = false;
	bool dc = false;
	unsigned int val;
	int ret;

	ret = regmap_read(smb->regmap, IRQSTAT_E, &val);
	if (ret < 0)
		return ret;

	/*
	 * Dc and usb are set depending on whether they are enabled in
	 * platform data _and_ whether corresponding undervoltage is set.
	 */
	if (smb->pdata->use_mains)
		dc = !(val & IRQSTAT_E_DCIN_UV_STAT);
	if (smb->pdata->use_usb)
		usb = !(val & IRQSTAT_E_USBIN_UV_STAT);

	ret = smb->mains_online != dc || smb->usb_online != usb;
	smb->mains_online = dc;
	smb->usb_online = usb;

	return ret;
}

/*
 * smb347_is_ps_online - returns whether input power source is connected
 * @smb: pointer to smb347 charger instance
 *
 * Returns %true if input power source is connected. Note that this is
 * dependent on what platform has configured for usable power sources. For
 * example if USB is disabled, this will return %false even if the USB cable
 * is connected.
 */
static bool smb347_is_ps_online(struct smb347_charger *smb)
{
	return smb->usb_online || smb->mains_online;
}

/**
 * smb347_charging_status - returns status of charging
 * @smb: pointer to smb347 charger instance
 *
 * Function returns charging status. %0 means no charging is in progress,
 * %1 means pre-charging, %2 fast-charging and %3 taper-charging.
 */
static int smb347_charging_status(struct smb347_charger *smb)
{
	unsigned int val;
	int ret;

	if (!smb347_is_ps_online(smb))
		return 0;

	ret = regmap_read(smb->regmap, STAT_C, &val);
	if (ret < 0)
		return 0;

	return (val & STAT_C_CHG_MASK) >> STAT_C_CHG_SHIFT;
}

static int smb347_charging_set(struct smb347_charger *smb, bool enable)
{
	int ret = 0;

	if (smb->pdata->enable_control != SMB347_CHG_ENABLE_SW) {
		dev_dbg(smb->dev, "charging enable/disable in SW disabled\n");
		return 0;
	}

	if (smb->charging_enabled != enable) {
		ret = regmap_update_bits(smb->regmap, CMD_A, CMD_A_CHG_ENABLED,
					 enable ? CMD_A_CHG_ENABLED : 0);
		if (!ret)
			smb->charging_enabled = enable;
	}

	return ret;
}

static inline int smb347_charging_enable(struct smb347_charger *smb)
{
	return smb347_charging_set(smb, true);
}

static inline int smb347_charging_disable(struct smb347_charger *smb)
{
	return smb347_charging_set(smb, false);
}

static int smb347_start_stop_charging(struct smb347_charger *smb)
{
	int ret;

	/*
	 * Depending on whether valid power source is connected or not, we
	 * disable or enable the charging. We do it manually because it
	 * depends on how the platform has configured the valid inputs.
	 */
	if (smb347_is_ps_online(smb)) {
		ret = smb347_charging_enable(smb);
		if (ret < 0)
			dev_err(smb->dev, "failed to enable charging\n");
	} else {
		ret = smb347_charging_disable(smb);
		if (ret < 0)
			dev_err(smb->dev, "failed to disable charging\n");
	}

	return ret;
}

static int smb347_set_charge_current(struct smb347_charger *smb)
{
	unsigned int id = smb->id;
	int ret;

	if (smb->pdata->max_charge_current) {
		ret = current_to_hw(fcc_tbl[id], ARRAY_SIZE(fcc_tbl[id]),
				    smb->pdata->max_charge_current);
		if (ret < 0)
			return ret;

		ret = regmap_update_bits(smb->regmap, CFG_CHARGE_CURRENT,
					 CFG_CHARGE_CURRENT_FCC_MASK,
					 ret << CFG_CHARGE_CURRENT_FCC_SHIFT);
		if (ret < 0)
			return ret;
	}

	if (smb->pdata->pre_charge_current) {
		ret = current_to_hw(pcc_tbl[id], ARRAY_SIZE(pcc_tbl[id]),
				    smb->pdata->pre_charge_current);
		if (ret < 0)
			return ret;

		ret = regmap_update_bits(smb->regmap, CFG_CHARGE_CURRENT,
					 CFG_CHARGE_CURRENT_PCC_MASK,
					 ret << CFG_CHARGE_CURRENT_PCC_SHIFT);
		if (ret < 0)
			return ret;
	}

	if (smb->pdata->termination_current) {
		ret = current_to_hw(tc_tbl[id], ARRAY_SIZE(tc_tbl[id]),
				    smb->pdata->termination_current);
		if (ret < 0)
			return ret;

		ret = regmap_update_bits(smb->regmap, CFG_CHARGE_CURRENT,
					 CFG_CHARGE_CURRENT_TC_MASK, ret);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int smb347_set_current_limits(struct smb347_charger *smb)
{
	unsigned int id = smb->id;
	int ret;

	if (smb->pdata->mains_current_limit) {
		ret = current_to_hw(icl_tbl[id], ARRAY_SIZE(icl_tbl[id]),
				    smb->pdata->mains_current_limit);
		if (ret < 0)
			return ret;

		ret = regmap_update_bits(smb->regmap, CFG_CURRENT_LIMIT,
					 CFG_CURRENT_LIMIT_DC_MASK,
					 ret << CFG_CURRENT_LIMIT_DC_SHIFT);
		if (ret < 0)
			return ret;
	}

	if (smb->pdata->usb_hc_current_limit) {
		ret = current_to_hw(icl_tbl[id], ARRAY_SIZE(icl_tbl[id]),
				    smb->pdata->usb_hc_current_limit);
		if (ret < 0)
			return ret;

		ret = regmap_update_bits(smb->regmap, CFG_CURRENT_LIMIT,
					 CFG_CURRENT_LIMIT_USB_MASK, ret);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int smb347_set_voltage_limits(struct smb347_charger *smb)
{
	int ret;

	if (smb->pdata->pre_to_fast_voltage) {
		ret = smb->pdata->pre_to_fast_voltage;

		/* uV */
		ret = clamp_val(ret, 2400000, 3000000) - 2400000;
		ret /= 200000;

		ret = regmap_update_bits(smb->regmap, CFG_FLOAT_VOLTAGE,
				CFG_FLOAT_VOLTAGE_THRESHOLD_MASK,
				ret << CFG_FLOAT_VOLTAGE_THRESHOLD_SHIFT);
		if (ret < 0)
			return ret;
	}

	if (smb->pdata->max_charge_voltage) {
		ret = smb->pdata->max_charge_voltage;

		/* uV */
		ret = clamp_val(ret, 3500000, 4500000) - 3500000;
		ret /= 20000;

		ret = regmap_update_bits(smb->regmap, CFG_FLOAT_VOLTAGE,
					 CFG_FLOAT_VOLTAGE_FLOAT_MASK, ret);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int smb347_set_temp_limits(struct smb347_charger *smb)
{
	unsigned int id = smb->id;
	bool enable_therm_monitor = false;
	int ret = 0;
	int val;

	if (smb->pdata->chip_temp_threshold) {
		val = smb->pdata->chip_temp_threshold;

		/* degree C */
		val = clamp_val(val, 100, 130) - 100;
		val /= 10;

		ret = regmap_update_bits(smb->regmap, CFG_OTG,
					 CFG_OTG_TEMP_THRESHOLD_MASK,
					 val << CFG_OTG_TEMP_THRESHOLD_SHIFT);
		if (ret < 0)
			return ret;
	}

	if (smb->pdata->soft_cold_temp_limit != SMB347_TEMP_USE_DEFAULT) {
		val = smb->pdata->soft_cold_temp_limit;

		val = clamp_val(val, 0, 15);
		val /= 5;
		/* this goes from higher to lower so invert the value */
		val = ~val & 0x3;

		ret = regmap_update_bits(smb->regmap, CFG_TEMP_LIMIT,
					 CFG_TEMP_LIMIT_SOFT_COLD_MASK,
					 val << CFG_TEMP_LIMIT_SOFT_COLD_SHIFT);
		if (ret < 0)
			return ret;

		enable_therm_monitor = true;
	}

	if (smb->pdata->soft_hot_temp_limit != SMB347_TEMP_USE_DEFAULT) {
		val = smb->pdata->soft_hot_temp_limit;

		val = clamp_val(val, 40, 55) - 40;
		val /= 5;

		ret = regmap_update_bits(smb->regmap, CFG_TEMP_LIMIT,
					 CFG_TEMP_LIMIT_SOFT_HOT_MASK,
					 val << CFG_TEMP_LIMIT_SOFT_HOT_SHIFT);
		if (ret < 0)
			return ret;

		enable_therm_monitor = true;
	}

	if (smb->pdata->hard_cold_temp_limit != SMB347_TEMP_USE_DEFAULT) {
		val = smb->pdata->hard_cold_temp_limit;

		val = clamp_val(val, -5, 10) + 5;
		val /= 5;
		/* this goes from higher to lower so invert the value */
		val = ~val & 0x3;

		ret = regmap_update_bits(smb->regmap, CFG_TEMP_LIMIT,
					 CFG_TEMP_LIMIT_HARD_COLD_MASK,
					 val << CFG_TEMP_LIMIT_HARD_COLD_SHIFT);
		if (ret < 0)
			return ret;

		enable_therm_monitor = true;
	}

	if (smb->pdata->hard_hot_temp_limit != SMB347_TEMP_USE_DEFAULT) {
		val = smb->pdata->hard_hot_temp_limit;

		val = clamp_val(val, 50, 65) - 50;
		val /= 5;

		ret = regmap_update_bits(smb->regmap, CFG_TEMP_LIMIT,
					 CFG_TEMP_LIMIT_HARD_HOT_MASK,
					 val << CFG_TEMP_LIMIT_HARD_HOT_SHIFT);
		if (ret < 0)
			return ret;

		enable_therm_monitor = true;
	}

	/*
	 * If any of the temperature limits are set, we also enable the
	 * thermistor monitoring.
	 *
	 * When soft limits are hit, the device will start to compensate
	 * current and/or voltage depending on the configuration.
	 *
	 * When hard limit is hit, the device will suspend charging
	 * depending on the configuration.
	 */
	if (enable_therm_monitor) {
		ret = regmap_update_bits(smb->regmap, CFG_THERM,
					 CFG_THERM_MONITOR_DISABLED, 0);
		if (ret < 0)
			return ret;
	}

	if (smb->pdata->suspend_on_hard_temp_limit) {
		ret = regmap_update_bits(smb->regmap, CFG_SYSOK,
				 CFG_SYSOK_SUSPEND_HARD_LIMIT_DISABLED, 0);
		if (ret < 0)
			return ret;
	}

	if (smb->pdata->soft_temp_limit_compensation !=
	    SMB347_SOFT_TEMP_COMPENSATE_DEFAULT) {
		val = smb->pdata->soft_temp_limit_compensation & 0x3;

		ret = regmap_update_bits(smb->regmap, CFG_THERM,
				 CFG_THERM_SOFT_HOT_COMPENSATION_MASK,
				 val << CFG_THERM_SOFT_HOT_COMPENSATION_SHIFT);
		if (ret < 0)
			return ret;

		ret = regmap_update_bits(smb->regmap, CFG_THERM,
				 CFG_THERM_SOFT_COLD_COMPENSATION_MASK,
				 val << CFG_THERM_SOFT_COLD_COMPENSATION_SHIFT);
		if (ret < 0)
			return ret;
	}

	if (smb->pdata->charge_current_compensation) {
		val = current_to_hw(ccc_tbl[id], ARRAY_SIZE(ccc_tbl[id]),
				    smb->pdata->charge_current_compensation);
		if (val < 0)
			return val;

		ret = regmap_update_bits(smb->regmap, CFG_OTG,
				CFG_OTG_CC_COMPENSATION_MASK,
				(val & 0x3) << CFG_OTG_CC_COMPENSATION_SHIFT);
		if (ret < 0)
			return ret;
	}

	return ret;
}

/*
 * smb347_set_writable - enables/disables writing to non-volatile registers
 * @smb: pointer to smb347 charger instance
 *
 * You can enable/disable writing to the non-volatile configuration
 * registers by calling this function.
 *
 * Returns %0 on success and negative errno in case of failure.
 */
static int smb347_set_writable(struct smb347_charger *smb, bool writable)
{
	return regmap_update_bits(smb->regmap, CMD_A, CMD_A_ALLOW_WRITE,
				  writable ? CMD_A_ALLOW_WRITE : 0);
}

static int smb347_hw_init(struct smb347_charger *smb)
{
	unsigned int val;
	int ret;

	ret = smb347_set_writable(smb, true);
	if (ret < 0)
		return ret;

	/*
	 * Program the platform specific configuration values to the device
	 * first.
	 */
	ret = smb347_set_charge_current(smb);
	if (ret < 0)
		goto fail;

	ret = smb347_set_current_limits(smb);
	if (ret < 0)
		goto fail;

	ret = smb347_set_voltage_limits(smb);
	if (ret < 0)
		goto fail;

	ret = smb347_set_temp_limits(smb);
	if (ret < 0)
		goto fail;

	/* If USB charging is disabled we put the USB in suspend mode */
	if (!smb->pdata->use_usb) {
		ret = regmap_update_bits(smb->regmap, CMD_A,
					 CMD_A_SUSPEND_ENABLED,
					 CMD_A_SUSPEND_ENABLED);
		if (ret < 0)
			goto fail;
	}

	/*
	 * If configured by platform data, we enable hardware Auto-OTG
	 * support for driving VBUS. Otherwise we disable it.
	 */
	ret = regmap_update_bits(smb->regmap, CFG_OTHER, CFG_OTHER_RID_MASK,
		smb->pdata->use_usb_otg ? CFG_OTHER_RID_ENABLED_AUTO_OTG : 0);
	if (ret < 0)
		goto fail;

	/*
	 * Make the charging functionality controllable by a write to the
	 * command register unless pin control is specified in the platform
	 * data.
	 */
	switch (smb->pdata->enable_control) {
	case SMB347_CHG_ENABLE_PIN_ACTIVE_LOW:
		val = CFG_PIN_EN_CTRL_ACTIVE_LOW;
		break;
	case SMB347_CHG_ENABLE_PIN_ACTIVE_HIGH:
		val = CFG_PIN_EN_CTRL_ACTIVE_HIGH;
		break;
	default:
		val = 0;
		break;
	}

	ret = regmap_update_bits(smb->regmap, CFG_PIN, CFG_PIN_EN_CTRL_MASK,
				 val);
	if (ret < 0)
		goto fail;

	/* Disable Automatic Power Source Detection (APSD) interrupt. */
	ret = regmap_update_bits(smb->regmap, CFG_PIN, CFG_PIN_EN_APSD_IRQ, 0);
	if (ret < 0)
		goto fail;

	ret = smb347_update_ps_status(smb);
	if (ret < 0)
		goto fail;

	ret = smb347_start_stop_charging(smb);

fail:
	smb347_set_writable(smb, false);
	return ret;
}

static irqreturn_t smb347_interrupt(int irq, void *data)
{
	struct smb347_charger *smb = data;
	unsigned int stat_c, irqstat_c, irqstat_d, irqstat_e;
	bool handled = false;
	int ret;

	/* SMB347 it needs at least 20ms for setting IRQSTAT_E_*IN_UV_IRQ */
	usleep_range(25000, 35000);

	ret = regmap_read(smb->regmap, STAT_C, &stat_c);
	if (ret < 0) {
		dev_warn(smb->dev, "reading STAT_C failed\n");
		return IRQ_NONE;
	}

	ret = regmap_read(smb->regmap, IRQSTAT_C, &irqstat_c);
	if (ret < 0) {
		dev_warn(smb->dev, "reading IRQSTAT_C failed\n");
		return IRQ_NONE;
	}

	ret = regmap_read(smb->regmap, IRQSTAT_D, &irqstat_d);
	if (ret < 0) {
		dev_warn(smb->dev, "reading IRQSTAT_D failed\n");
		return IRQ_NONE;
	}

	ret = regmap_read(smb->regmap, IRQSTAT_E, &irqstat_e);
	if (ret < 0) {
		dev_warn(smb->dev, "reading IRQSTAT_E failed\n");
		return IRQ_NONE;
	}

	/*
	 * If we get charger error we report the error back to user.
	 * If the error is recovered charging will resume again.
	 */
	if (stat_c & STAT_C_CHARGER_ERROR) {
		dev_err(smb->dev, "charging stopped due to charger error\n");
		if (smb->pdata->use_mains)
			power_supply_changed(smb->mains);
		if (smb->pdata->use_usb)
			power_supply_changed(smb->usb);
		handled = true;
	}

	/*
	 * If we reached the termination current the battery is charged and
	 * we can update the status now. Charging is automatically
	 * disabled by the hardware.
	 */
	if (irqstat_c & (IRQSTAT_C_TERMINATION_IRQ | IRQSTAT_C_TAPER_IRQ)) {
		if (irqstat_c & IRQSTAT_C_TERMINATION_STAT) {
			if (smb->pdata->use_mains)
				power_supply_changed(smb->mains);
			if (smb->pdata->use_usb)
				power_supply_changed(smb->usb);
		}
		dev_dbg(smb->dev, "going to HW maintenance mode\n");
		handled = true;
	}

	/*
	 * If we got a charger timeout INT that means the charge
	 * full is not detected with in charge timeout value.
	 */
	if (irqstat_d & IRQSTAT_D_CHARGE_TIMEOUT_IRQ) {
		dev_dbg(smb->dev, "total Charge Timeout INT received\n");

		if (irqstat_d & IRQSTAT_D_CHARGE_TIMEOUT_STAT)
			dev_warn(smb->dev, "charging stopped due to timeout\n");
		if (smb->pdata->use_mains)
			power_supply_changed(smb->mains);
		if (smb->pdata->use_usb)
			power_supply_changed(smb->usb);
		handled = true;
	}

	/*
	 * If we got an under voltage interrupt it means that AC/USB input
	 * was connected or disconnected.
	 */
	if (irqstat_e & (IRQSTAT_E_USBIN_UV_IRQ | IRQSTAT_E_DCIN_UV_IRQ)) {
		if (smb347_update_ps_status(smb) > 0) {
			smb347_start_stop_charging(smb);
			if (smb->pdata->use_mains)
				power_supply_changed(smb->mains);
			if (smb->pdata->use_usb)
				power_supply_changed(smb->usb);
		}
		handled = true;
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
}

static int smb347_irq_set(struct smb347_charger *smb, bool enable)
{
	int ret;

	ret = smb347_set_writable(smb, true);
	if (ret < 0)
		return ret;

	/*
	 * Enable/disable interrupts for:
	 *	- under voltage
	 *	- termination current reached
	 *	- charger timeout
	 *	- charger error
	 */
	ret = regmap_update_bits(smb->regmap, CFG_FAULT_IRQ, 0xff,
				 enable ? CFG_FAULT_IRQ_DCIN_UV : 0);
	if (ret < 0)
		goto fail;

	ret = regmap_update_bits(smb->regmap, CFG_STATUS_IRQ, 0xff,
			enable ? (CFG_STATUS_IRQ_TERMINATION_OR_TAPER |
					CFG_STATUS_IRQ_CHARGE_TIMEOUT) : 0);
	if (ret < 0)
		goto fail;

	ret = regmap_update_bits(smb->regmap, CFG_PIN, CFG_PIN_EN_CHARGER_ERROR,
				 enable ? CFG_PIN_EN_CHARGER_ERROR : 0);
fail:
	smb347_set_writable(smb, false);
	return ret;
}

static inline int smb347_irq_enable(struct smb347_charger *smb)
{
	return smb347_irq_set(smb, true);
}

static inline int smb347_irq_disable(struct smb347_charger *smb)
{
	return smb347_irq_set(smb, false);
}

static int smb347_irq_init(struct smb347_charger *smb,
			   struct i2c_client *client)
{
	const struct smb347_charger_platform_data *pdata = smb->pdata;
	unsigned long irqflags = IRQF_ONESHOT;
	int ret;

	/* Requesting GPIO for IRQ is only needed in non-DT way */
	if (!client->irq) {
		int irq = gpio_to_irq(pdata->irq_gpio);

		ret = devm_gpio_request_one(smb->dev, pdata->irq_gpio,
					    GPIOF_IN, client->name);
		if (ret < 0)
			return ret;

		irqflags |= IRQF_TRIGGER_FALLING;
		client->irq = irq;
	}

	ret = devm_request_threaded_irq(smb->dev, client->irq, NULL,
					smb347_interrupt, irqflags,
					client->name, smb);
	if (ret < 0)
		return ret;

	ret = smb347_set_writable(smb, true);
	if (ret < 0)
		return ret;

	/*
	 * Configure the STAT output to be suitable for interrupts: disable
	 * all other output (except interrupts) and make it active low.
	 */
	ret = regmap_update_bits(smb->regmap, CFG_STAT,
				 CFG_STAT_ACTIVE_HIGH | CFG_STAT_DISABLED,
				 CFG_STAT_DISABLED);
	if (ret < 0)
		client->irq = 0;

	smb347_set_writable(smb, false);

	return ret;
}

/*
 * Returns the constant charge current programmed
 * into the charger in uA.
 */
static int get_const_charge_current(struct smb347_charger *smb)
{
	unsigned int id = smb->id;
	int ret, intval;
	unsigned int v;

	if (!smb347_is_ps_online(smb))
		return -ENODATA;

	ret = regmap_read(smb->regmap, STAT_B, &v);
	if (ret < 0)
		return ret;

	/*
	 * The current value is composition of FCC and PCC values
	 * and we can detect which table to use from bit 5.
	 */
	if (v & 0x20) {
		intval = hw_to_current(fcc_tbl[id],
				       ARRAY_SIZE(fcc_tbl[id]), v & 7);
	} else {
		v >>= 3;
		intval = hw_to_current(pcc_tbl[id],
				       ARRAY_SIZE(pcc_tbl[id]), v & 7);
	}

	return intval;
}

/*
 * Returns the constant charge voltage programmed
 * into the charger in uV.
 */
static int get_const_charge_voltage(struct smb347_charger *smb)
{
	int ret, intval;
	unsigned int v;

	if (!smb347_is_ps_online(smb))
		return -ENODATA;

	ret = regmap_read(smb->regmap, STAT_A, &v);
	if (ret < 0)
		return ret;

	v &= STAT_A_FLOAT_VOLTAGE_MASK;
	if (v > 0x3d)
		v = 0x3d;

	intval = 3500000 + v * 20000;

	return intval;
}

static int smb347_get_charging_status(struct smb347_charger *smb,
				      struct power_supply *psy)
{
	int ret, status;
	unsigned int val;

	if (psy->desc->type == POWER_SUPPLY_TYPE_USB) {
		if (!smb->usb_online)
			return POWER_SUPPLY_STATUS_DISCHARGING;
	} else {
		if (!smb->mains_online)
			return POWER_SUPPLY_STATUS_DISCHARGING;
	}

	ret = regmap_read(smb->regmap, STAT_C, &val);
	if (ret < 0)
		return ret;

	if ((val & STAT_C_CHARGER_ERROR) ||
			(val & STAT_C_HOLDOFF_STAT)) {
		/*
		 * set to NOT CHARGING upon charger error
		 * or charging has stopped.
		 */
		status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	} else {
		if ((val & STAT_C_CHG_MASK) >> STAT_C_CHG_SHIFT) {
			/*
			 * set to charging if battery is in pre-charge,
			 * fast charge or taper charging mode.
			 */
			status = POWER_SUPPLY_STATUS_CHARGING;
		} else if (val & STAT_C_CHG_TERM) {
			/*
			 * set the status to FULL if battery is not in pre
			 * charge, fast charge or taper charging mode AND
			 * charging is terminated at least once.
			 */
			status = POWER_SUPPLY_STATUS_FULL;
		} else {
			/*
			 * in this case no charger error or termination
			 * occured but charging is not in progress!!!
			 */
			status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		}
	}

	return status;
}

static int smb347_get_property_locked(struct power_supply *psy,
				      enum power_supply_property prop,
				      union power_supply_propval *val)
{
	struct smb347_charger *smb = power_supply_get_drvdata(psy);
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = smb347_get_charging_status(smb, psy);
		if (ret < 0)
			return ret;
		val->intval = ret;
		break;

	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		if (psy->desc->type == POWER_SUPPLY_TYPE_USB) {
			if (!smb->usb_online)
				return -ENODATA;
		} else {
			if (!smb->mains_online)
				return -ENODATA;
		}

		/*
		 * We handle trickle and pre-charging the same, and taper
		 * and none the same.
		 */
		switch (smb347_charging_status(smb)) {
		case 1:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
			break;
		case 2:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
			break;
		default:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
			break;
		}
		break;

	case POWER_SUPPLY_PROP_ONLINE:
		if (psy->desc->type == POWER_SUPPLY_TYPE_USB)
			val->intval = smb->usb_online;
		else
			val->intval = smb->mains_online;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = get_const_charge_voltage(smb);
		if (ret < 0)
			return ret;
		val->intval = ret;
		break;

	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = get_const_charge_current(smb);
		if (ret < 0)
			return ret;
		val->intval = ret;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int smb347_get_property(struct power_supply *psy,
			       enum power_supply_property prop,
			       union power_supply_propval *val)
{
	struct smb347_charger *smb = power_supply_get_drvdata(psy);
	struct i2c_client *client = to_i2c_client(smb->dev);
	int ret;

	disable_irq(client->irq);
	ret = smb347_get_property_locked(psy, prop, val);
	enable_irq(client->irq);

	return ret;
}

static enum power_supply_property smb347_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
};

static bool smb347_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case IRQSTAT_A:
	case IRQSTAT_C:
	case IRQSTAT_D:
	case IRQSTAT_E:
	case IRQSTAT_F:
	case STAT_A:
	case STAT_B:
	case STAT_C:
	case STAT_E:
		return true;
	}

	return false;
}

static bool smb347_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CFG_CHARGE_CURRENT:
	case CFG_CURRENT_LIMIT:
	case CFG_FLOAT_VOLTAGE:
	case CFG_STAT:
	case CFG_PIN:
	case CFG_THERM:
	case CFG_SYSOK:
	case CFG_OTHER:
	case CFG_OTG:
	case CFG_TEMP_LIMIT:
	case CFG_FAULT_IRQ:
	case CFG_STATUS_IRQ:
	case CFG_ADDRESS:
	case CMD_A:
	case CMD_B:
	case CMD_C:
		return true;
	}

	return smb347_volatile_reg(dev, reg);
}

static void smb347_dt_parse_pdata(struct device_node *np,
				  struct smb347_charger_platform_data *pdata)
{
	pdata->soft_temp_limit_compensation =
					SMB347_SOFT_TEMP_COMPENSATE_DEFAULT;
	/*
	 * These properties come from the battery info, still we need to
	 * pre-initialize the values. See smb347_get_battery_info() below.
	 */
	pdata->soft_cold_temp_limit = SMB347_TEMP_USE_DEFAULT;
	pdata->hard_cold_temp_limit = SMB347_TEMP_USE_DEFAULT;
	pdata->soft_hot_temp_limit  = SMB347_TEMP_USE_DEFAULT;
	pdata->hard_hot_temp_limit  = SMB347_TEMP_USE_DEFAULT;

	/* Charging constraints */
	of_property_read_u32(np, "summit,fast-voltage-threshold-microvolt",
			     &pdata->pre_to_fast_voltage);
	of_property_read_u32(np, "summit,mains-current-limit-microamp",
			     &pdata->mains_current_limit);
	of_property_read_u32(np, "summit,usb-current-limit-microamp",
			     &pdata->usb_hc_current_limit);

	/* For thermometer monitoring */
	of_property_read_u32(np, "summit,chip-temperature-threshold-celsius",
			     &pdata->chip_temp_threshold);
	of_property_read_u32(np, "summit,soft-compensation-method",
			     &pdata->soft_temp_limit_compensation);
	of_property_read_u32(np, "summit,charge-current-compensation-microamp",
			     &pdata->charge_current_compensation);

	/* Supported charging mode */
	pdata->use_mains =
		of_property_read_bool(np, "summit,enable-mains-charging");
	pdata->use_usb =
		of_property_read_bool(np, "summit,enable-usb-charging");
	pdata->use_usb_otg =
		of_property_read_bool(np, "summit,enable-otg-charging");

	/* Select charging control */
	of_property_read_u32(np, "summit,enable-charge-control",
			     &pdata->enable_control);

	/* Interrupt support is optional */
	if (!of_find_property(np, "interrupts", NULL))
		pdata->irq_gpio = -1;
}

static int smb347_get_battery_info(struct smb347_charger *smb)
{
	struct smb347_charger_platform_data *pdata = (void *)smb->pdata;
	struct power_supply_battery_info info = {};
	struct power_supply *supply;
	int err;

	if (smb->mains)
		supply = smb->mains;
	else
		supply = smb->usb;

	err = power_supply_get_battery_info(supply, &info);
	if (err == -ENXIO || err == -ENODEV)
		return 0;
	if (err)
		return err;

	if (info.constant_charge_current_max_ua != -EINVAL)
		pdata->max_charge_current = info.constant_charge_current_max_ua;

	if (info.constant_charge_voltage_max_uv != -EINVAL)
		pdata->max_charge_voltage = info.constant_charge_voltage_max_uv;

	if (info.precharge_current_ua != -EINVAL)
		pdata->pre_charge_current = info.precharge_current_ua;

	if (info.charge_term_current_ua != -EINVAL)
		pdata->termination_current = info.charge_term_current_ua;

	if (info.temp_alert_min != INT_MIN)
		pdata->soft_cold_temp_limit = info.temp_alert_min;

	if (info.temp_alert_max != INT_MAX)
		pdata->soft_hot_temp_limit = info.temp_alert_max;

	if (info.temp_min != INT_MIN)
		pdata->hard_cold_temp_limit = info.temp_min;

	if (info.temp_max != INT_MAX)
		pdata->hard_hot_temp_limit = info.temp_max;

	/* Suspend when battery temperature is outside hard limits */
	if (pdata->hard_cold_temp_limit != SMB347_TEMP_USE_DEFAULT ||
	    pdata->hard_hot_temp_limit != SMB347_TEMP_USE_DEFAULT)
		pdata->suspend_on_hard_temp_limit = true;

	return 0;
}

static struct smb347_charger_platform_data
			*smb347_get_platdata(struct device *dev)
{
	struct smb347_charger_platform_data *pdata;

	if (dev->of_node) {
		pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
		if (pdata)
			smb347_dt_parse_pdata(dev->of_node, pdata);
	} else {
		pdata = dev_get_platdata(dev);
	}

	return pdata;
}

static const struct regmap_config smb347_regmap = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= SMB347_MAX_REGISTER,
	.volatile_reg	= smb347_volatile_reg,
	.readable_reg	= smb347_readable_reg,
};

static const struct power_supply_desc smb347_mains_desc = {
	.name		= "smb347-mains",
	.type		= POWER_SUPPLY_TYPE_MAINS,
	.get_property	= smb347_get_property,
	.properties	= smb347_properties,
	.num_properties	= ARRAY_SIZE(smb347_properties),
};

static const struct power_supply_desc smb347_usb_desc = {
	.name		= "smb347-usb",
	.type		= POWER_SUPPLY_TYPE_USB,
	.get_property	= smb347_get_property,
	.properties	= smb347_properties,
	.num_properties	= ARRAY_SIZE(smb347_properties),
};

static int smb347_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct power_supply_config mains_usb_cfg = {};
	struct device *dev = &client->dev;
	struct smb347_charger *smb;
	int ret;

	smb = devm_kzalloc(dev, sizeof(*smb), GFP_KERNEL);
	if (!smb)
		return -ENOMEM;

	smb->pdata = smb347_get_platdata(dev);
	if (!smb->pdata)
		return -ENODEV;

	if (!smb->pdata->use_mains && !smb->pdata->use_usb)
		return -EINVAL;

	i2c_set_clientdata(client, smb);

	smb->dev = &client->dev;
	smb->id = id->driver_data;

	smb->regmap = devm_regmap_init_i2c(client, &smb347_regmap);
	if (IS_ERR(smb->regmap))
		return PTR_ERR(smb->regmap);

	mains_usb_cfg.drv_data = smb;
	mains_usb_cfg.of_node = dev->of_node;
	if (smb->pdata->use_mains) {
		smb->mains = devm_power_supply_register(dev, &smb347_mains_desc,
							&mains_usb_cfg);
		if (IS_ERR(smb->mains))
			return PTR_ERR(smb->mains);
	}

	if (smb->pdata->use_usb) {
		smb->usb = devm_power_supply_register(dev, &smb347_usb_desc,
						      &mains_usb_cfg);
		if (IS_ERR(smb->usb))
			return PTR_ERR(smb->usb);
	}

	ret = smb347_get_battery_info(smb);
	if (ret)
		return ret;

	ret = smb347_hw_init(smb);
	if (ret < 0)
		return ret;

	/*
	 * Interrupt pin is optional. If it is connected, we setup the
	 * interrupt support here.
	 */
	if (smb->pdata->irq_gpio >= 0) {
		ret = smb347_irq_init(smb, client);
		if (ret < 0) {
			dev_warn(dev, "failed to initialize IRQ: %d\n", ret);
			dev_warn(dev, "disabling IRQ support\n");
		} else {
			smb347_irq_enable(smb);
		}
	}

	return 0;
}

static int smb347_remove(struct i2c_client *client)
{
	struct smb347_charger *smb = i2c_get_clientdata(client);

	if (client->irq)
		smb347_irq_disable(smb);
	return 0;
}

static const struct i2c_device_id smb347_id[] = {
	{ "smb345", SMB345 },
	{ "smb347", SMB347 },
	{ "smb358", SMB358 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, smb347_id);

static const struct of_device_id smb3xx_of_match[] = {
	{ .compatible = "summit,smb345" },
	{ .compatible = "summit,smb347" },
	{ .compatible = "summit,smb358" },
	{ },
};
MODULE_DEVICE_TABLE(of, smb3xx_of_match);

static struct i2c_driver smb347_driver = {
	.driver = {
		.name = "smb347",
		.of_match_table = smb3xx_of_match,
	},
	.probe        = smb347_probe,
	.remove       = smb347_remove,
	.id_table     = smb347_id,
};

module_i2c_driver(smb347_driver);

MODULE_AUTHOR("Bruce E. Robertson <bruce.e.robertson@intel.com>");
MODULE_AUTHOR("Mika Westerberg <mika.westerberg@linux.intel.com>");
MODULE_DESCRIPTION("SMB347 battery charger driver");
MODULE_LICENSE("GPL");
