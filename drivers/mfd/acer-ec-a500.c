// SPDX-License-Identifier: GPL-2.0+
/*
 * MFD driver for Acer Iconia Tab A500 Embedded Controller.
 *
 * Copyright 2020 GRATE-driver project.
 *
 * Based on downstream driver from Acer Inc.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/irqflags.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/reboot.h>

#include <linux/mfd/acer-ec-a500.h>
#include <linux/mfd/core.h>

#define A500_EC_I2C_ERR_TIMEOUT		500

/*				cmd	delay ms */
A500_EC_COMMAND(SHUTDOWN,	0x52,	1000);
A500_EC_COMMAND(WARM_REBOOT,	0x54,	1000);
A500_EC_COMMAND(COLD_REBOOT,	0x55,	1000);

static struct a500_ec *a500_ec_scratch;

static void a500_ec_delay(unsigned int delay_ms)
{
	/* interrupts could be disabled on shutdown or reboot */
	if (irqs_disabled())
		mdelay(delay_ms);
	else
		msleep(delay_ms);
}

int a500_ec_read_locked(struct a500_ec *ec_chip,
			const struct a500_ec_cmd *cmd_desc)
{
	struct i2c_client *client = ec_chip->client;
	unsigned int retries = 5;
	s32 ret = 0;

	while (retries-- > 0) {
		ret = i2c_smbus_read_word_data(client, cmd_desc->cmd);
		if (ret >= 0)
			break;

		a500_ec_delay(A500_EC_I2C_ERR_TIMEOUT);
	}

	if (ret < 0) {
		dev_err(&client->dev, "i2c read command 0x%x failed: %d\n",
			cmd_desc->cmd, ret);
		return ret;
	}

	a500_ec_delay(cmd_desc->post_delay);

	return le16_to_cpu(ret);
}
EXPORT_SYMBOL_GPL(a500_ec_read_locked);

int a500_ec_write_locked(struct a500_ec *ec_chip,
			 const struct a500_ec_cmd *cmd_desc, u16 value)
{
	struct i2c_client *client = ec_chip->client;
	unsigned int retries = 5;
	s32 ret = 0;

	while (retries-- > 0) {
		ret = i2c_smbus_write_word_data(client, cmd_desc->cmd,
						le16_to_cpu(value));
		if (ret >= 0)
			break;

		a500_ec_delay(A500_EC_I2C_ERR_TIMEOUT);
	}

	if (ret < 0) {
		dev_err(&client->dev, "i2c write command 0x%x failed: %d\n",
			cmd_desc->cmd, ret);
		return ret;
	}

	a500_ec_delay(cmd_desc->post_delay);

	return 0;
}
EXPORT_SYMBOL_GPL(a500_ec_write_locked);

static void a500_ec_poweroff(void)
{
	struct a500_ec *ec_chip = a500_ec_scratch;

	a500_ec_write_locked(ec_chip, SHUTDOWN, 0);
}

static int a500_ec_restart_notify(struct notifier_block *this,
				  unsigned long reboot_mode, void *data)
{
	struct a500_ec *ec_chip = a500_ec_scratch;

	if (reboot_mode == REBOOT_WARM)
		a500_ec_write_locked(ec_chip, WARM_REBOOT, 0);
	else
		a500_ec_write_locked(ec_chip, COLD_REBOOT, 1);

	return NOTIFY_DONE;
}

static struct notifier_block a500_ec_restart_handler = {
	.notifier_call = a500_ec_restart_notify,
	.priority = 200,
};

static const struct mfd_cell a500_ec_cells[] = {
	{ .name = "acer-a500-iconia-battery", },
	{ .name = "acer-a500-iconia-leds", },
};

static int a500_ec_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct a500_ec *ec_chip;
	int err;

	ec_chip = devm_kzalloc(&client->dev, sizeof(*ec_chip), GFP_KERNEL);
	if (!ec_chip)
		return -ENOMEM;

	ec_chip->client = client;
	mutex_init(&ec_chip->lock);

	i2c_set_clientdata(client, ec_chip);

	/* register battery and LED sub-devices */
	err = devm_mfd_add_devices(&client->dev, PLATFORM_DEVID_AUTO,
				   a500_ec_cells, ARRAY_SIZE(a500_ec_cells),
				   NULL, 0, NULL);
	if (err) {
		dev_err(&client->dev, "failed to add sub-devices: %d\n", err);
		return err;
	}

	/* set up power management functions */
	if (of_device_is_system_power_controller(client->dev.of_node)) {
		a500_ec_scratch = ec_chip;

		err = register_restart_handler(&a500_ec_restart_handler);
		if (err) {
			dev_err(&client->dev,
				"unable to register restart handler: %d\n",
				err);
			return err;
		}

		if (!pm_power_off)
			pm_power_off = a500_ec_poweroff;
	}

	return 0;
}

static int a500_ec_remove(struct i2c_client *client)
{
	if (of_device_is_system_power_controller(client->dev.of_node)) {
		if (pm_power_off == a500_ec_poweroff)
			pm_power_off = NULL;

		unregister_restart_handler(&a500_ec_restart_handler);
	}

	return 0;
}

static const struct of_device_id a500_ec_match[] = {
	{ .compatible = "acer,a500-iconia-ec" },
	{ }
};
MODULE_DEVICE_TABLE(of, a500_ec_match);

static struct i2c_driver a500_ec_driver = {
	.driver = {
		.name = "acer-a500-embedded-controller",
		.of_match_table = a500_ec_match,
	},
	.probe = a500_ec_probe,
	.remove = a500_ec_remove,
};
module_i2c_driver(a500_ec_driver);

MODULE_DESCRIPTION("Acer Iconia Tab A500 Embedded Controller driver");
MODULE_AUTHOR("Dmitry Osipenko <digetx@gmail.com>");
MODULE_LICENSE("GPL v2");
