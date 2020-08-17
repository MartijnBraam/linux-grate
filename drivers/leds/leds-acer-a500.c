// SPDX-License-Identifier: GPL-2.0+
/*
 * Power button LED driver for Acer Iconia Tab A500.
 *
 * Copyright 2020 GRATE-driver project.
 */

#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <linux/mfd/acer-ec-a500.h>

struct a500_ec_led {
	struct led_classdev cdev;
	struct a500_ec_led *other_led;
	const struct a500_ec_cmd *cmd;
};

/*					cmd	delay ms */
A500_EC_COMMAND(RESET_LEDS,		0x40,	100);
A500_EC_COMMAND(POWER_LED_ON,		0x42,	100);
A500_EC_COMMAND(CHARGE_LED_ON,		0x43,	100);
A500_EC_COMMAND(ANDROID_LEDS_OFF,	0x5A,	100);

static int a500_ec_led_brightness_set(struct led_classdev *led_cdev,
				      enum led_brightness value)
{
	struct device *a500_ec_leds_dev = led_cdev->dev->parent;
	struct a500_ec *ec_chip = dev_get_drvdata(a500_ec_leds_dev->parent);
	struct a500_ec_led *led = container_of(led_cdev, struct a500_ec_led,
					       cdev);
	int ret;

	a500_ec_lock(ec_chip);

	if (value) {
		ret = a500_ec_write_locked(ec_chip, led->cmd, 0);
	} else {
		/*
		 * There is no separate controls which can disable LEDs
		 * individually, there is only RESET_LEDS command that turns
		 * off both LEDs.
		 */
		ret = a500_ec_write_locked(ec_chip, RESET_LEDS, 0);
		if (ret)
			goto unlock;

		led = led->other_led;

		/* RESET_LEDS turns off both LEDs, thus restore other LED */
		if (led->cdev.brightness == LED_ON)
			ret = a500_ec_write_locked(ec_chip, led->cmd, 0);
	}

unlock:
	a500_ec_unlock(ec_chip);

	return ret;
}

static int a500_ec_leds_probe(struct platform_device *pdev)
{
	struct a500_ec *ec_chip = dev_get_drvdata(pdev->dev.parent);
	struct a500_ec_led *white_led, *orange_led;
	int err;

	/* reset and turn off all LEDs */
	a500_ec_write(ec_chip, RESET_LEDS, 0);
	a500_ec_write(ec_chip, ANDROID_LEDS_OFF, 0);

	white_led = devm_kzalloc(&pdev->dev, sizeof(*white_led), GFP_KERNEL);
	if (!white_led)
		return -ENOMEM;

	white_led->cdev.name = "power-button-white",
	white_led->cdev.brightness_set_blocking = a500_ec_led_brightness_set;
	white_led->cdev.flags = LED_CORE_SUSPENDRESUME;
	white_led->cdev.max_brightness = LED_ON;
	white_led->cmd = &A500_EC_POWER_LED_ON;

	orange_led = devm_kzalloc(&pdev->dev, sizeof(*orange_led), GFP_KERNEL);
	if (!orange_led)
		return -ENOMEM;

	orange_led->cdev.name = "power-button-orange",
	orange_led->cdev.brightness_set_blocking = a500_ec_led_brightness_set;
	orange_led->cdev.flags = LED_CORE_SUSPENDRESUME;
	orange_led->cdev.max_brightness = LED_ON;
	orange_led->cmd = &A500_EC_CHARGE_LED_ON;

	white_led->other_led = orange_led;
	orange_led->other_led = white_led;

	err = devm_led_classdev_register(&pdev->dev, &white_led->cdev);
	if (err) {
		dev_err(&pdev->dev, "failed to register white LED\n");
		return err;
	}

	err = devm_led_classdev_register(&pdev->dev, &orange_led->cdev);
	if (err) {
		dev_err(&pdev->dev, "failed to register orange LED\n");
		return err;
	}

	return 0;
}

static struct platform_driver a500_ec_leds_driver = {
	.driver = {
		.name = "acer-a500-iconia-leds",
	},
	.probe = a500_ec_leds_probe,
};
module_platform_driver(a500_ec_leds_driver);

MODULE_DESCRIPTION("LED driver for Acer Iconia Tab A500 Power Button");
MODULE_AUTHOR("Dmitry Osipenko <digetx@gmail.com>");
MODULE_ALIAS("platform:acer-a500-iconia-leds");
MODULE_LICENSE("GPL v2");
