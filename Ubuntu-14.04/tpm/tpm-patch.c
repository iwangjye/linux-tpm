/*
 * Copyright (C) 2004 IBM Corporation
 *
 * Authors:
 * Leendert van Doorn <leendert@watson.ibm.com>
 * Dave Safford <safford@watson.ibm.com>
 * Reiner Sailer <sailer@watson.ibm.com>
 * Kylene Hall <kjhall@us.ibm.com>
 *
 * Maintained by: <tpmdd-devel@lists.sourceforge.net>
 *
 * Device driver for TCG/TCPA TPM (trusted platform module).
 * Specifications at www.trustedcomputinggroup.org	 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 * 
 * Note, the TPM chip is not interrupt driven (only polling)
 * and can have very long timeouts (minutes!). Hence the unusual
 * calls to msleep.
 *
 */

#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/freezer.h>

#include "tpm.h"
#include "tpm_eventlog.h"

enum tpm_duration {
	TPM_SHORT = 0,
	TPM_MEDIUM = 1,
	TPM_LONG = 2,
	TPM_UNDEFINED,
};

#define TPM_MAX_ORDINAL 243
#define TSC_MAX_ORDINAL 12
#define TPM_PROTECTED_COMMAND 0x00
#define TPM_CONNECTION_COMMAND 0x40

/*
 * Bug workaround - some TPM's don't flush the most
 * recently changed pcr on suspend, so force the flush
 * with an extend to the selected _unused_ non-volatile pcr.
 */
static int tpm_suspend_pcr;
module_param_named(suspend_pcr, tpm_suspend_pcr, uint, 0644);
MODULE_PARM_DESC(suspend_pcr,
		 "PCR to use for dummy writes to faciltate flush on suspend.");

static LIST_HEAD(tpm_chip_list);
static DEFINE_SPINLOCK(driver_lock);
static DECLARE_BITMAP(dev_mask, TPM_NUM_DEVICES);

/*
 * Array with one entry per ordinal defining the maximum amount
 * of time the chip could take to return the result.  The ordinal
 * designation of short, medium or long is defined in a table in
 * TCG Specification TPM Main Part 2 TPM Structures Section 17. The
 * values of the SHORT, MEDIUM, and LONG durations are retrieved
 * from the chip during initialization with a call to tpm_get_timeouts.
 */
static const u8 tpm_ordinal_duration[TPM_MAX_ORDINAL] = {
	TPM_UNDEFINED,		/* 0 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 5 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 10 */
	TPM_SHORT,
	TPM_MEDIUM,
	TPM_LONG,
	TPM_LONG,
	TPM_MEDIUM,		/* 15 */
	TPM_SHORT,
	TPM_SHORT,
	TPM_MEDIUM,
	TPM_LONG,
	TPM_SHORT,		/* 20 */
	TPM_SHORT,
	TPM_MEDIUM,
	TPM_MEDIUM,
	TPM_MEDIUM,
	TPM_SHORT,		/* 25 */
	TPM_SHORT,
	TPM_MEDIUM,
	TPM_SHORT,
	TPM_SHORT,
	TPM_MEDIUM,		/* 30 */
	TPM_LONG,
	TPM_MEDIUM,
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,		/* 35 */
	TPM_MEDIUM,
	TPM_MEDIUM,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_MEDIUM,		/* 40 */
	TPM_LONG,
	TPM_MEDIUM,
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,		/* 45 */
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_LONG,
	TPM_MEDIUM,		/* 50 */
	TPM_MEDIUM,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 55 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_MEDIUM,		/* 60 */
	TPM_MEDIUM,
	TPM_MEDIUM,
	TPM_SHORT,
	TPM_SHORT,
	TPM_MEDIUM,		/* 65 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 70 */
	TPM_SHORT,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 75 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_LONG,		/* 80 */
	TPM_UNDEFINED,
	TPM_MEDIUM,
	TPM_LONG,
	TPM_SHORT,
	TPM_UNDEFINED,		/* 85 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 90 */
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_UNDEFINED,		/* 95 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_MEDIUM,		/* 100 */
	TPM_SHORT,
	TPM_SHORT,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 105 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 110 */
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,		/* 115 */
	TPM_SHORT,
	TPM_SHORT,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_LONG,		/* 120 */
	TPM_LONG,
	TPM_MEDIUM,
	TPM_UNDEFINED,
	TPM_SHORT,
	TPM_SHORT,		/* 125 */
	TPM_SHORT,
	TPM_LONG,
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,		/* 130 */
	TPM_MEDIUM,
	TPM_UNDEFINED,
	TPM_SHORT,
	TPM_MEDIUM,
	TPM_UNDEFINED,		/* 135 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 140 */
	TPM_SHORT,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 145 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 150 */
	TPM_MEDIUM,
	TPM_MEDIUM,
	TPM_SHORT,
	TPM_SHORT,
	TPM_UNDEFINED,		/* 155 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 160 */
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 165 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_LONG,		/* 170 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 175 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_MEDIUM,		/* 180 */
	TPM_SHORT,
	TPM_MEDIUM,
	TPM_MEDIUM,
	TPM_MEDIUM,
	TPM_MEDIUM,		/* 185 */
	TPM_SHORT,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 190 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 195 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 200 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,
	TPM_SHORT,		/* 205 */
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_MEDIUM,		/* 210 */
	TPM_UNDEFINED,
	TPM_MEDIUM,
	TPM_MEDIUM,
	TPM_MEDIUM,
	TPM_UNDEFINED,		/* 215 */
	TPM_MEDIUM,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,
	TPM_SHORT,		/* 220 */
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_SHORT,
	TPM_UNDEFINED,		/* 225 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 230 */
	TPM_LONG,
	TPM_MEDIUM,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,		/* 235 */
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_UNDEFINED,
	TPM_SHORT,		/* 240 */
	TPM_UNDEFINED,
	TPM_MEDIUM,
};


int tpm_release(struct inode *inode, struct file *file)
{
	struct tpm_chip *chip = file->private_data;

	del_singleshot_timer_sync(&chip->user_read_timer);
	flush_work(&chip->work);
	file->private_data = NULL;
	atomic_set(&chip->data_pending, 0);
	kzfree(chip->data_buffer);
	clear_bit(0, &chip->is_open);
	put_device(chip->dev);
	return 0;
}
EXPORT_SYMBOL_GPL(tpm_release);

int tpm_open(struct inode *inode, struct file *file)
{
	int minor = iminor(inode);
	struct tpm_chip *chip = NULL, *pos;

	rcu_read_lock();
	list_for_each_entry_rcu(pos, &tpm_chip_list, list) {
		if (pos->vendor.miscdev.minor == minor) {
			chip = pos;
			get_device(chip->dev);
			break;
		}
	}
	rcu_read_unlock();

	if (!chip)
		return -ENODEV;

	if (test_and_set_bit(0, &chip->is_open)) {
		dev_dbg(chip->dev, "Another process owns this TPM\n");
		put_device(chip->dev);
		return -EBUSY;
	}

	chip->data_buffer = kzalloc(TPM_BUFSIZE, GFP_KERNEL);
	if (chip->data_buffer == NULL) {
		clear_bit(0, &chip->is_open);
		put_device(chip->dev);
		return -ENOMEM;
	}

	atomic_set(&chip->data_pending, 0);

	file->private_data = chip;
	return 0;
}
EXPORT_SYMBOL_GPL(tpm_open);

ssize_t tpm_write(struct file *file, const char __user *buf,
		  size_t size, loff_t *off)
{
	struct tpm_chip *chip = file->private_data;
	size_t in_size = size;
	ssize_t out_size;

	/* cannot perform a write until the read has cleared
	   either via tpm_read or a user_read_timer timeout.
	   This also prevents splitted buffered writes from blocking here.
	*/
	if (atomic_read(&chip->data_pending) != 0)
		return -EBUSY;

	if (in_size > TPM_BUFSIZE)
		return -E2BIG;

	mutex_lock(&chip->buffer_mutex);

	if (copy_from_user
	    (chip->data_buffer, (void __user *) buf, in_size)) {
		mutex_unlock(&chip->buffer_mutex);
		return -EFAULT;
	}

	/* atomic tpm command send and result receive */
	out_size = tpm_transmit(chip, chip->data_buffer, TPM_BUFSIZE);
	if (out_size < 0) {
		mutex_unlock(&chip->buffer_mutex);
		return out_size;
	}

	atomic_set(&chip->data_pending, out_size);
	mutex_unlock(&chip->buffer_mutex);

	/* Set a timeout by which the reader must come claim the result */
	mod_timer(&chip->user_read_timer, jiffies + (60 * HZ));

	return in_size;
}
EXPORT_SYMBOL_GPL(tpm_write);

ssize_t tpm_read(struct file *file, char __user *buf,
		 size_t size, loff_t *off)
{
	struct tpm_chip *chip = file->private_data;
	ssize_t ret_size;
	int rc;

	del_singleshot_timer_sync(&chip->user_read_timer);
	flush_work(&chip->work);
	ret_size = atomic_read(&chip->data_pending);
	if (ret_size > 0) {	/* relay data */
		ssize_t orig_ret_size = ret_size;
		if (size < ret_size)
			ret_size = size;

		mutex_lock(&chip->buffer_mutex);
		rc = copy_to_user(buf, chip->data_buffer, ret_size);
		memset(chip->data_buffer, 0, orig_ret_size);
		if (rc)
			ret_size = -EFAULT;

		mutex_unlock(&chip->buffer_mutex);
	}

	atomic_set(&chip->data_pending, 0);

	return ret_size;
}
EXPORT_SYMBOL_GPL(tpm_read);

ssize_t tpm2_show_ownerauth(struct device * dev, struct device_attribute * attr,
            char *buf)
{
    ssize_t rc;
    tpm2_cap_t cap;
	struct tpms_gatged_prop *prop;

    rc = __tpm2_cap(dev, TPM_CAP_TPM_PROPERTIES,
                TPM_PT_PERMANENT, &cap);
	if (rc > 0)
		prop = cap.tpm2_properties.tpm2_property;
	rc = sprintf(buf, "%d\n", (be32_to_cpu(prop->value)
                & OWNER_AUTH_BIT) ? 1 : 0);
    return rc;
}
EXPORT_SYMBOL_GPL(tpm2_show_ownerauth);

ssize_t tpm2_show_endorseauth(struct device * dev, struct device_attribute * attr,
            char *buf)
{
    ssize_t rc;
    tpm2_cap_t cap;
    struct tpms_gatged_prop *prop;

    rc = __tpm2_cap(dev, TPM_CAP_TPM_PROPERTIES,
                TPM_PT_PERMANENT, &cap);
    if (rc > 0)
        prop = cap.tpm2_properties.tpm2_property;
	rc = sprintf(buf, "%d\n", (be32_to_cpu(prop->value)
                & ENDORSEMENT_AUTH_BIT) ? 1 : 0);
    return rc;
}
EXPORT_SYMBOL_GPL(tpm2_show_endorseauth);

ssize_t tpm2_show_phenable(struct device * dev, struct device_attribute * attr,
            char *buf)
{
    ssize_t rc;
    tpm2_cap_t cap;
    struct tpms_gatged_prop *prop;

    rc = __tpm2_cap(dev, TPM_CAP_TPM_PROPERTIES,
                TPM_PT_STARTUP_CLEAR, &cap);
    if (rc > 0)
        prop = cap.tpm2_properties.tpm2_property;
	rc = sprintf(buf, "%d\n", (be32_to_cpu(prop->value)
                & PH_ENABLE_BIT) ? 1 : 0);
    return rc;
}
EXPORT_SYMBOL_GPL(tpm2_show_phenable);

ssize_t tpm2_show_shenable(struct device * dev, struct device_attribute * attr,
            char *buf)
{
    ssize_t rc;
    tpm2_cap_t cap;
    struct tpms_gatged_prop *prop;

    rc = __tpm2_cap(dev, TPM_CAP_TPM_PROPERTIES,
                TPM_PT_STARTUP_CLEAR, &cap);
    if (rc > 0)
        prop = cap.tpm2_properties.tpm2_property;
	rc = sprintf(buf, "%d\n", (be32_to_cpu(prop->value)
                & SH_ENABLE_BIT) ? 1 : 0);
    return rc;
}
EXPORT_SYMBOL_GPL(tpm2_show_shenable);

ssize_t tpm2_show_ehenable(struct device * dev, struct device_attribute * attr,
            char *buf)
{
    ssize_t rc;
    tpm2_cap_t cap;
    struct tpms_gatged_prop *prop;

    rc = __tpm2_cap(dev, TPM_CAP_TPM_PROPERTIES,
                TPM_PT_STARTUP_CLEAR, &cap);
    if (rc > 0)
        prop = cap.tpm2_properties.tpm2_property;
    rc = sprintf(buf, "%d\n", (be32_to_cpu(prop->value)
                & EH_ENABLE_BIT) ? 1 : 0);
    return rc;
}
EXPORT_SYMBOL_GPL(tpm2_show_ehenable);

ssize_t tpm2_show_pcrs(struct device * dev, struct device_attribute * attr,
            char *buf)
{
    ssize_t rc;
    tpm2_cap_t cap;
    __be32 max_pcr;
    u8 i, j;
    u8 digest[SHA1_DIGEST_SIZE];
	struct tpms_gatged_prop *prop;
    char *str = buf;

    rc = __tpm2_cap(dev, TPM_CAP_TPM_PROPERTIES,
                TPM_PT_PCR_COUNT, &cap);
	if (rc > 0)
        prop = cap.tpm2_properties.tpm2_property;
    max_pcr = prop->value;
    if (be32_to_cpu(max_pcr) != IMPLEMENTATION_PCR)
        return -1;

    for(i =0; i< be32_to_cpu(max_pcr); i++) {
        rc = __tpm2_pcr_read(dev, i, digest);
        if (rc < 0)
            break;
        str += sprintf(str, "PCR-%02u :", i);
        for(j = 0; j < SHA1_DIGEST_SIZE; j++)
            str += sprintf(str, "%02x ", digest[j]);
        str += sprintf(str, "\n");
    }

    return str - buf;
}
EXPORT_SYMBOL_GPL(tpm2_show_pcrs);

void tpm_remove_hardware(struct device *dev)
{
	struct tpm_chip *chip = dev_get_drvdata(dev);

	if (chip == NULL) {
		dev_err(dev, "No device data found\n");
		return;
	}

	spin_lock(&driver_lock);
	list_del_rcu(&chip->list);
	spin_unlock(&driver_lock);
	synchronize_rcu();

	misc_deregister(&chip->vendor.miscdev);
	sysfs_remove_group(&dev->kobj, chip->vendor.attr_group);
	tpm_remove_ppi(&dev->kobj);
	tpm_bios_log_teardown(chip->bios_dir);

	/* write it this way to be explicit (chip->dev == dev) */
	put_device(chip->dev);
}
EXPORT_SYMBOL_GPL(tpm_remove_hardware);

struct tpm_chip *tpm_register_hardware(struct device *dev,
					const struct tpm_vendor_specific *entry)
{
#define DEVNAME_SIZE 7

	char *devname;
	struct tpm_chip *chip;

	/* Driver specific per-device data */
	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	devname = kmalloc(DEVNAME_SIZE, GFP_KERNEL);

	if (chip == NULL || devname == NULL)
		goto out_free;

	mutex_init(&chip->buffer_mutex);
	mutex_init(&chip->tpm_mutex);
	INIT_LIST_HEAD(&chip->list);

	INIT_WORK(&chip->work, timeout_work);

	setup_timer(&chip->user_read_timer, user_reader_timeout,
			(unsigned long)chip);

	memcpy(&chip->vendor, entry, sizeof(struct tpm_vendor_specific));

	chip->dev_num = find_first_zero_bit(dev_mask, TPM_NUM_DEVICES);

	if (chip->dev_num >= TPM_NUM_DEVICES) {
		dev_err(dev, "No available tpm device numbers\n");
		goto out_free;
	} else if (chip->dev_num == 0)
		chip->vendor.miscdev.minor = TPM_MINOR;
	else
		chip->vendor.miscdev.minor = MISC_DYNAMIC_MINOR;

	set_bit(chip->dev_num, dev_mask);

	scnprintf(devname, DEVNAME_SIZE, "%s%d", "tpm", chip->dev_num);
	chip->vendor.miscdev.name = devname;

	chip->vendor.miscdev.parent = dev;
	chip->dev = get_device(dev);
	chip->release = dev->release;
	dev->release = tpm_dev_release;
	dev_set_drvdata(dev, chip);

	if (misc_register(&chip->vendor.miscdev)) {
		dev_err(chip->dev,
			"unable to misc_register %s, minor %d\n",
			chip->vendor.miscdev.name,
			chip->vendor.miscdev.minor);
		goto put_device;
	}

	if (sysfs_create_group(&dev->kobj, chip->vendor.attr_group)) {
		misc_deregister(&chip->vendor.miscdev);
		goto put_device;
	}

	if (tpm_add_ppi(&dev->kobj)) {
		misc_deregister(&chip->vendor.miscdev);
		goto put_device;
	}

	chip->bios_dir = tpm_bios_log_setup(devname);

	/* Make chip available */
	spin_lock(&driver_lock);
	list_add_rcu(&chip->list, &tpm_chip_list);
	spin_unlock(&driver_lock);

	return chip;

put_device:
	put_device(chip->dev);
out_free:
	kfree(chip);
	kfree(devname);
	return NULL;
}
EXPORT_SYMBOL_GPL(tpm_register_hardware);