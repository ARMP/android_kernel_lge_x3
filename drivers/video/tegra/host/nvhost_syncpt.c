/*
 * drivers/video/tegra/host/nvhost_syncpt.c
 *
 * Tegra Graphics Host Syncpoints
 *
 * Copyright (c) 2010-2012, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/nvhost_ioctl.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/console.h>
#include <linux/io.h>
#include <linux/syscalls.h>
#include <linux/kmod.h>
#include <linux/string.h>

#include <asm/thread_info.h>
#include <asm/uaccess.h>
#include <asm/system.h>

#include "nvhost_syncpt.h"
#include "dev.h"

#define MAX_STUCK_CHECK_COUNT 15

void debug_stuck_syncpoint(void)
{
	char buf[20] = "/d/clock/clock_tree";
	struct file *clock_file = filp_open(buf, O_RDWR, 0);
	loff_t offset = 0;
	ssize_t count = 0;

	mm_segment_t clock_fs;

	clock_fs = get_fs();
	set_fs(KERNEL_DS);

	
	if (IS_ERR(clock_file)) {
		printk("%s: Failed to open %s\n", __func__, buf);
	} else {
		printk("%s: Opened %s\n", __func__, buf);
		if (clock_file->f_op != NULL && clock_file->f_op->read != NULL)
		{
			printk("%s: Reading %s\n", __func__, buf);
			do {
				count = clock_file->f_op->read(clock_file,
							buf,
							strlen(buf),
							&offset);
				printk("%s", buf);
			} while(count);
			printk("\n");
		}
		filp_close(clock_file, NULL);
	}

	set_fs(clock_fs);
}

/**
 * Resets syncpoint and waitbase values to sw shadows
 */
void nvhost_syncpt_reset(struct nvhost_syncpt *sp)
{
	u32 i;
	BUG_ON(!(syncpt_op(sp).reset && syncpt_op(sp).reset_wait_base));

	for (i = 0; i < sp->nb_pts; i++)
		syncpt_op(sp).reset(sp, i);
	for (i = 0; i < sp->nb_bases; i++)
		syncpt_op(sp).reset_wait_base(sp, i);
	wmb();
}

/**
 * Updates sw shadow state for client managed registers
 */
void nvhost_syncpt_save(struct nvhost_syncpt *sp)
{
	u32 i;
	BUG_ON(!(syncpt_op(sp).update_min && syncpt_op(sp).read_wait_base));

	for (i = 0; i < sp->nb_pts; i++) {
		if (client_managed(i))
			syncpt_op(sp).update_min(sp, i);
		else
			BUG_ON(!nvhost_syncpt_min_eq_max(sp, i));
	}

	for (i = 0; i < sp->nb_bases; i++)
		syncpt_op(sp).read_wait_base(sp, i);
}

/**
 * Updates the last value read from hardware.
 */
u32 nvhost_syncpt_update_min(struct nvhost_syncpt *sp, u32 id)
{
	BUG_ON(!syncpt_op(sp).update_min);

	return syncpt_op(sp).update_min(sp, id);
}

/**
 * Get the current syncpoint value
 */
u32 nvhost_syncpt_read(struct nvhost_syncpt *sp, u32 id)
{
	u32 val;
	BUG_ON(!syncpt_op(sp).update_min);
	nvhost_module_busy(syncpt_to_dev(sp)->dev);
	val = syncpt_op(sp).update_min(sp, id);
	nvhost_module_idle(syncpt_to_dev(sp)->dev);
	return val;
}

/**
 * Get the current syncpoint base
 */
u32 nvhost_syncpt_read_wait_base(struct nvhost_syncpt *sp, u32 id)
{
	u32 val;
	BUG_ON(!syncpt_op(sp).read_wait_base);
	nvhost_module_busy(syncpt_to_dev(sp)->dev);
	syncpt_op(sp).read_wait_base(sp, id);
	val = sp->base_val[id];
	nvhost_module_idle(syncpt_to_dev(sp)->dev);
	return val;
}

/**
 * Write a cpu syncpoint increment to the hardware, without touching
 * the cache. Caller is responsible for host being powered.
 */
void nvhost_syncpt_cpu_incr(struct nvhost_syncpt *sp, u32 id)
{
	BUG_ON(!syncpt_op(sp).cpu_incr);
	syncpt_op(sp).cpu_incr(sp, id);
}

/**
 * Increment syncpoint value from cpu, updating cache
 */
void nvhost_syncpt_incr(struct nvhost_syncpt *sp, u32 id)
{
	nvhost_syncpt_incr_max(sp, id, 1);
	nvhost_module_busy(syncpt_to_dev(sp)->dev);
	nvhost_syncpt_cpu_incr(sp, id);
	nvhost_module_idle(syncpt_to_dev(sp)->dev);
}

/**
 * Main entrypoint for syncpoint value waits.
 */
int nvhost_syncpt_wait_timeout(struct nvhost_syncpt *sp, u32 id,
			u32 thresh, u32 timeout, u32 *value)
{
	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wq);
	void *ref;
	void *waiter;
	int err = 0, check_count = 0, low_timeout = 0;
	static int print_once = 0;

	if (value)
		*value = 0;

	BUG_ON(!syncpt_op(sp).update_min);
	if (!nvhost_syncpt_check_max(sp, id, thresh)) {
		dev_warn(&syncpt_to_dev(sp)->pdev->dev,
			"wait %d (%s) for (%d) wouldn't be met (max %d)\n",
			id, syncpt_op(sp).name(sp, id), thresh,
			nvhost_syncpt_read_max(sp, id));
		nvhost_debug_dump(syncpt_to_dev(sp));
		return -EINVAL;
	}

	/* first check cache */
	if (nvhost_syncpt_min_cmp(sp, id, thresh)) {
		if (value)
			*value = nvhost_syncpt_read_min(sp, id);
		return 0;
	}

	/* keep host alive */
	nvhost_module_busy(syncpt_to_dev(sp)->dev);

	if (client_managed(id) || !nvhost_syncpt_min_eq_max(sp, id)) {
		/* try to read from register */
		u32 val = syncpt_op(sp).update_min(sp, id);
		if ((s32)(val - thresh) >= 0) {
			if (value)
				*value = val;
			goto done;
		}
	}

	if (!timeout) {
		err = -EAGAIN;
		goto done;
	}

	/* schedule a wakeup when the syncpoint value is reached */
	waiter = nvhost_intr_alloc_waiter();
	if (!waiter) {
		err = -ENOMEM;
		goto done;
	}

	err = nvhost_intr_add_action(&(syncpt_to_dev(sp)->intr), id, thresh,
				NVHOST_INTR_ACTION_WAKEUP_INTERRUPTIBLE, &wq,
				waiter,
				&ref);
	if (err)
		goto done;

	err = -EAGAIN;
	/* wait for the syncpoint, or timeout, or signal */
	while (timeout) {
		u32 check = min_t(u32, SYNCPT_CHECK_PERIOD, timeout);
		int remain = wait_event_interruptible_timeout(wq,
						nvhost_syncpt_min_cmp(sp, id, thresh),
						check);
		if (remain > 0 || nvhost_syncpt_min_cmp(sp, id, thresh)) {
			if (value)
				*value = nvhost_syncpt_read_min(sp, id);
			err = 0;
			break;
		}
		if (remain < 0) {
			err = remain;
			break;
		}
		if (timeout != NVHOST_NO_TIMEOUT) {
			if (timeout < SYNCPT_CHECK_PERIOD) {
				/* Caller-specified timeout may be impractically low */
				low_timeout = timeout;
			}
			timeout -= check;
		}
		if (timeout) {
			dev_warn(&syncpt_to_dev(sp)->pdev->dev,
				"%s: syncpoint id %d (%s) stuck waiting %d, timeout=%d\n",
				 current->comm, id, syncpt_op(sp).name(sp, id),
				 thresh, timeout);
			syncpt_op(sp).debug(sp);
			print_once++;
			if (print_once == 1)
			{
				nvhost_debug_dump(syncpt_to_dev(sp));
				debug_stuck_syncpoint();
			}
			if (check_count > MAX_STUCK_CHECK_COUNT) {
				if (low_timeout) {
					dev_warn(&syncpt_to_dev(sp)->pdev->dev,
						"is timeout %d too low?\n",
						low_timeout);
				}
				nvhost_debug_dump(syncpt_to_dev(sp));
				BUG();
			}
			check_count++;
		}
	}
	nvhost_intr_put_ref(&(syncpt_to_dev(sp)->intr), ref);

done:
	nvhost_module_idle(syncpt_to_dev(sp)->dev);
	return err;
}

void nvhost_syncpt_debug(struct nvhost_syncpt *sp)
{
	syncpt_op(sp).debug(sp);
}

int nvhost_mutex_try_lock(struct nvhost_syncpt *sp, int idx)
{
	struct nvhost_master *host = syncpt_to_dev(sp);
	u32 reg;

	nvhost_module_busy(host->dev);
	reg = syncpt_op(sp).mutex_try_lock(sp, idx);
	if (reg) {
		nvhost_module_idle(host->dev);
		return -EBUSY;
	}
	atomic_inc(&sp->lock_counts[idx]);
	return 0;
}

void nvhost_mutex_unlock(struct nvhost_syncpt *sp, int idx)
{
	syncpt_op(sp).mutex_unlock(sp, idx);
	nvhost_module_idle(syncpt_to_dev(sp)->dev);
	atomic_dec(&sp->lock_counts[idx]);
}

/* check for old WAITs to be removed (avoiding a wrap) */
int nvhost_syncpt_wait_check(struct nvhost_syncpt *sp,
			     struct nvmap_client *nvmap,
			     u32 waitchk_mask,
			     struct nvhost_waitchk *wait,
			     int num_waitchk)
{
	return syncpt_op(sp).wait_check(sp, nvmap,
			waitchk_mask, wait, num_waitchk);
}