/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * API for Embedded Controller of Acer Iconia Tab A500.
 *
 * Copyright 2020 GRATE-driver project.
 */

#ifndef __LINUX_MFD_ACER_A500_EC_H
#define __LINUX_MFD_ACER_A500_EC_H

#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct a500_ec_cmd {
	unsigned int cmd;
	unsigned int post_delay;
};

#define A500_EC_COMMAND(NAME, CMD, DELAY_MS)				\
static const struct a500_ec_cmd A500_EC_##NAME = {			\
	.cmd = CMD,							\
	.post_delay = DELAY_MS,						\
};									\
static const __maybe_unused struct a500_ec_cmd *NAME = &A500_EC_##NAME

struct a500_ec {
	struct i2c_client *client;

	/*
	 * Some EC commands shall be executed sequentially and some commands
	 * shall not be executed instantly after the other commands. Hence the
	 * locking is needed in order to protect from conflicting accesses to
	 * the EC.
	 */
	struct mutex lock;
};

int a500_ec_read_locked(struct a500_ec *ec_chip,
			const struct a500_ec_cmd *cmd_desc);

int a500_ec_write_locked(struct a500_ec *ec_chip,
			 const struct a500_ec_cmd *cmd_desc, u16 value);

static inline void a500_ec_lock(struct a500_ec *ec_chip)
{
	mutex_lock(&ec_chip->lock);
}

static inline void a500_ec_unlock(struct a500_ec *ec_chip)
{
	mutex_unlock(&ec_chip->lock);
}

static inline int a500_ec_read(struct a500_ec *ec_chip,
			       const struct a500_ec_cmd *cmd_desc)
{
	s32 ret;

	a500_ec_lock(ec_chip);
	ret = a500_ec_read_locked(ec_chip, cmd_desc);
	a500_ec_unlock(ec_chip);

	return ret;
}

static inline int a500_ec_write(struct a500_ec *ec_chip,
				const struct a500_ec_cmd *cmd_desc,
				u16 value)
{
	s32 ret;

	a500_ec_lock(ec_chip);
	ret = a500_ec_write_locked(ec_chip, cmd_desc, value);
	a500_ec_unlock(ec_chip);

	return ret;
}

#endif /* __LINUX_MFD_ACER_A500_EC_H */
