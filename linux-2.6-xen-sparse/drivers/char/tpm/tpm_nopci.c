/*
 * Copyright (C) 2004 IBM Corporation
 *
 * Authors:
 * Leendert van Doorn <leendert@watson.ibm.com>
 * Dave Safford <safford@watson.ibm.com>
 * Reiner Sailer <sailer@watson.ibm.com>
 * Kylene Hall <kjhall@us.ibm.com>
 *
 * Maintained by: <tpmdd_devel@lists.sourceforge.net>
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
 * calls to schedule_timeout.
 *
 */

#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include "tpm_nopci.h"

enum {
	TPM_MINOR = 224,	/* officially assigned */
	TPM_BUFSIZE = 2048,
	TPM_NUM_DEVICES = 256,
	TPM_NUM_MASK_ENTRIES = TPM_NUM_DEVICES / (8 * sizeof(int))
};

  /* PCI configuration addresses */
enum {
	PCI_GEN_PMCON_1 = 0xA0,
	PCI_GEN1_DEC = 0xE4,
	PCI_LPC_EN = 0xE6,
	PCI_GEN2_DEC = 0xEC
};

enum {
	TPM_LOCK_REG = 0x0D,
	TPM_INTERUPT_REG = 0x0A,
	TPM_BASE_ADDR_LO = 0x08,
	TPM_BASE_ADDR_HI = 0x09,
	TPM_UNLOCK_VALUE = 0x55,
	TPM_LOCK_VALUE = 0xAA,
	TPM_DISABLE_INTERUPT_VALUE = 0x00
};

static LIST_HEAD(tpm_chip_list);
static spinlock_t driver_lock = SPIN_LOCK_UNLOCKED;
static int dev_mask[32];

static void user_reader_timeout(unsigned long ptr)
{
	struct tpm_chip *chip = (struct tpm_chip *) ptr;

	down(&chip->buffer_mutex);
	atomic_set(&chip->data_pending, 0);
	memset(chip->data_buffer, 0, TPM_BUFSIZE);
	up(&chip->buffer_mutex);
}

void tpm_time_expired(unsigned long ptr)
{
	int *exp = (int *) ptr;
	*exp = 1;
}

EXPORT_SYMBOL_GPL(tpm_time_expired);


/*
 * This function should be used by other kernel subsystems attempting to use the tpm through the tpm_transmit interface.
 * A call to this function will return the chip structure corresponding to the TPM you are looking for that can then be sent with your command to tpm_transmit.
 * Passing 0 as the argument corresponds to /dev/tpm0 and thus the first and probably primary TPM on the system.  Passing 1 corresponds to /dev/tpm1 and the next TPM discovered.  If a TPM with the given chip_num does not exist NULL will be returned.
 */
struct tpm_chip* tpm_chip_lookup(int chip_num)
{

	struct tpm_chip *pos;
	list_for_each_entry(pos, &tpm_chip_list, list)
		if (pos->dev_num == chip_num ||
		    chip_num == TPM_ANY_NUM)
			return pos;

	return NULL;

}

/*
 * Internal kernel interface to transmit TPM commands
 */
ssize_t tpm_transmit(struct tpm_chip * chip, const char *buf,
		     size_t bufsiz)
{
	ssize_t rc;
	u32 count;
	unsigned long stop;

	count = be32_to_cpu(*((__be32 *) (buf + 2)));

	if (count == 0)
		return -ENODATA;
	if (count > bufsiz) {
		dev_err(chip->dev,
			"invalid count value %x %x \n", count, bufsiz);
		return -E2BIG;
	}

	dev_dbg(chip->dev, "TPM Ordinal: %d\n",
		be32_to_cpu(*((__be32 *) (buf + 6))));
	dev_dbg(chip->dev, "Chip Status: %x\n",
		inb(chip->vendor->base + 1));

	down(&chip->tpm_mutex);

	if ((rc = chip->vendor->send(chip, (u8 *) buf, count)) < 0) {
		dev_err(chip->dev,
			"tpm_transmit: tpm_send: error %d\n", rc);
		goto out;
	}

	stop = jiffies + 2 * 60 * HZ;
	do {
		u8 status = chip->vendor->status(chip);
		if ((status & chip->vendor->req_complete_mask) ==
		    chip->vendor->req_complete_val) {
			goto out_recv;
		}

		if ((status == chip->vendor->req_canceled)) {
			dev_err(chip->dev, "Operation Canceled\n");
			rc = -ECANCELED;
			goto out;
		}

		msleep(TPM_TIMEOUT);	/* CHECK */
		rmb();
	}
	while (time_before(jiffies, stop));


	chip->vendor->cancel(chip);
	dev_err(chip->dev, "Operation Timed out\n");
	rc = -ETIME;
	goto out;

out_recv:
	rc = chip->vendor->recv(chip, (u8 *) buf, bufsiz);
	if (rc < 0)
		dev_err(chip->dev,
			"tpm_transmit: tpm_recv: error %d\n", rc);
	atomic_set(&chip->data_position, 0);

out:
	up(&chip->tpm_mutex);
	return rc;
}

EXPORT_SYMBOL_GPL(tpm_transmit);

#define TPM_DIGEST_SIZE 20
#define CAP_PCR_RESULT_SIZE 18
static const u8 cap_pcr[] = {
	0, 193,			/* TPM_TAG_RQU_COMMAND */
	0, 0, 0, 22,		/* length */
	0, 0, 0, 101,		/* TPM_ORD_GetCapability */
	0, 0, 0, 5,
	0, 0, 0, 4,
	0, 0, 1, 1
};

#define READ_PCR_RESULT_SIZE 30
static const u8 pcrread[] = {
	0, 193,			/* TPM_TAG_RQU_COMMAND */
	0, 0, 0, 14,		/* length */
	0, 0, 0, 21,		/* TPM_ORD_PcrRead */
	0, 0, 0, 0		/* PCR index */
};

ssize_t tpm_show_pcrs(struct device *dev, char *buf)
{
	u8 data[READ_PCR_RESULT_SIZE];
	ssize_t len;
	int i, j, num_pcrs;
	__be32 index;
	char *str = buf;

	struct tpm_chip *chip = dev_get_drvdata(dev);
	if (chip == NULL)
		return -ENODEV;

	memcpy(data, cap_pcr, sizeof(cap_pcr));
	if ((len = tpm_transmit(chip, data, sizeof(data)))
	    < CAP_PCR_RESULT_SIZE)
		return len;

	num_pcrs = be32_to_cpu(*((__be32 *) (data + 14)));

	for (i = 0; i < num_pcrs; i++) {
		memcpy(data, pcrread, sizeof(pcrread));
		index = cpu_to_be32(i);
		memcpy(data + 10, &index, 4);
		if ((len = tpm_transmit(chip, data, sizeof(data)))
		    < READ_PCR_RESULT_SIZE)
			return len;
		str += sprintf(str, "PCR-%02d: ", i);
		for (j = 0; j < TPM_DIGEST_SIZE; j++)
			str += sprintf(str, "%02X ", *(data + 10 + j));
		str += sprintf(str, "\n");
	}
	return str - buf;
}

EXPORT_SYMBOL_GPL(tpm_show_pcrs);

/*
 * Return 0 on success.  On error pass along error code.
 * chip_id Upper 2 bytes equal ANY, HW_ONLY or SW_ONLY
 * Lower 2 bytes equal tpm idx # or AN&
 * res_buf must fit a TPM_PCR (20 bytes) or NULL if you don't care
 */
int tpm_pcr_read( u32 chip_id, int pcr_idx, u8* res_buf, int res_buf_size )
{
	u8 data[READ_PCR_RESULT_SIZE];
	int rc;
	__be32 index;
	int chip_num = chip_id & TPM_CHIP_NUM_MASK;
	struct tpm_chip* chip;

	if ( res_buf && res_buf_size < TPM_DIGEST_SIZE )
		return -ENOSPC;
	if ( (chip = tpm_chip_lookup( chip_num /*,
				       chip_id >> TPM_CHIP_TYPE_SHIFT*/ ) ) == NULL ) {
		printk("chip %d not found.\n",chip_num);
		return -ENODEV;
	}
	memcpy(data, pcrread, sizeof(pcrread));
	index = cpu_to_be32(pcr_idx);
	memcpy(data + 10, &index, 4);
	if ((rc = tpm_transmit(chip, data, sizeof(data))) > 0 )
		rc = be32_to_cpu(*((u32*)(data+6)));

	if ( rc == 0 && res_buf )
		memcpy(res_buf, data+10, TPM_DIGEST_SIZE);
	return rc;
}
EXPORT_SYMBOL_GPL(tpm_pcr_read);

#define EXTEND_PCR_SIZE 34
static const u8 pcrextend[] = {
	0, 193,		 		 		 /* TPM_TAG_RQU_COMMAND */
	0, 0, 0, 34,		 		 /* length */
	0, 0, 0, 20,		 		 /* TPM_ORD_Extend */
	0, 0, 0, 0		 		 /* PCR index */
};

/*
 * Return 0 on success.  On error pass along error code.
 * chip_id Upper 2 bytes equal ANY, HW_ONLY or SW_ONLY
 * Lower 2 bytes equal tpm idx # or ANY
 */
int tpm_pcr_extend(u32 chip_id, int pcr_idx, const u8* hash)
{
	u8 data[EXTEND_PCR_SIZE];
	int rc;
	__be32 index;
	int chip_num = chip_id & TPM_CHIP_NUM_MASK;
	struct tpm_chip* chip;

	if ( (chip = tpm_chip_lookup( chip_num /*,
				      chip_id >> TPM_CHIP_TYPE_SHIFT */)) == NULL )
		return -ENODEV;

	memcpy(data, pcrextend, sizeof(pcrextend));
	index = cpu_to_be32(pcr_idx);
	memcpy(data + 10, &index, 4);
	memcpy( data + 14, hash, TPM_DIGEST_SIZE );
	if ((rc = tpm_transmit(chip, data, sizeof(data))) > 0 )
		rc = be32_to_cpu(*((u32*)(data+6)));
	return rc;
}
EXPORT_SYMBOL_GPL(tpm_pcr_extend);



#define  READ_PUBEK_RESULT_SIZE 314
static const u8 readpubek[] = {
	0, 193,			/* TPM_TAG_RQU_COMMAND */
	0, 0, 0, 30,		/* length */
	0, 0, 0, 124,		/* TPM_ORD_ReadPubek */
};

ssize_t tpm_show_pubek(struct device *dev, char *buf)
{
	u8 *data;
	ssize_t len;
	int i, rc;
	char *str = buf;

	struct tpm_chip *chip = dev_get_drvdata(dev);
	if (chip == NULL)
		return -ENODEV;

	data = kmalloc(READ_PUBEK_RESULT_SIZE, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	memcpy(data, readpubek, sizeof(readpubek));
	memset(data + sizeof(readpubek), 0, 20);	/* zero nonce */

	if ((len = tpm_transmit(chip, data, READ_PUBEK_RESULT_SIZE)) <
	    READ_PUBEK_RESULT_SIZE) {
		rc = len;
		goto out;
	}

	/*
	   ignore header 10 bytes
	   algorithm 32 bits (1 == RSA )
	   encscheme 16 bits
	   sigscheme 16 bits
	   parameters (RSA 12->bytes: keybit, #primes, expbit)
	   keylenbytes 32 bits
	   256 byte modulus
	   ignore checksum 20 bytes
	 */

	str +=
	    sprintf(str,
		    "Algorithm: %02X %02X %02X %02X\nEncscheme: %02X %02X\n"
		    "Sigscheme: %02X %02X\nParameters: %02X %02X %02X %02X"
		    " %02X %02X %02X %02X %02X %02X %02X %02X\n"
		    "Modulus length: %d\nModulus: \n",
		    data[10], data[11], data[12], data[13], data[14],
		    data[15], data[16], data[17], data[22], data[23],
		    data[24], data[25], data[26], data[27], data[28],
		    data[29], data[30], data[31], data[32], data[33],
		    be32_to_cpu(*((__be32 *) (data + 32))));

	for (i = 0; i < 256; i++) {
		str += sprintf(str, "%02X ", data[i + 39]);
		if ((i + 1) % 16 == 0)
			str += sprintf(str, "\n");
	}
	rc = str - buf;
out:
	kfree(data);
	return rc;
}

EXPORT_SYMBOL_GPL(tpm_show_pubek);

#define CAP_VER_RESULT_SIZE 18
static const u8 cap_version[] = {
	0, 193,			/* TPM_TAG_RQU_COMMAND */
	0, 0, 0, 18,		/* length */
	0, 0, 0, 101,		/* TPM_ORD_GetCapability */
	0, 0, 0, 6,
	0, 0, 0, 0
};

#define CAP_MANUFACTURER_RESULT_SIZE 18
static const u8 cap_manufacturer[] = {
	0, 193,			/* TPM_TAG_RQU_COMMAND */
	0, 0, 0, 22,		/* length */
	0, 0, 0, 101,		/* TPM_ORD_GetCapability */
	0, 0, 0, 5,
	0, 0, 0, 4,
	0, 0, 1, 3
};

ssize_t tpm_show_caps(struct device *dev, char *buf)
{
	u8 data[sizeof(cap_manufacturer)];
	ssize_t len;
	char *str = buf;

	struct tpm_chip *chip = dev_get_drvdata(dev);
	if (chip == NULL)
		return -ENODEV;

	memcpy(data, cap_manufacturer, sizeof(cap_manufacturer));

	if ((len = tpm_transmit(chip, data, sizeof(data))) <
	    CAP_MANUFACTURER_RESULT_SIZE)
		return len;

	str += sprintf(str, "Manufacturer: 0x%x\n",
		       be32_to_cpu(*((__be32 *)(data + 14))));

	memcpy(data, cap_version, sizeof(cap_version));

	if ((len = tpm_transmit(chip, data, sizeof(data))) <
	    CAP_VER_RESULT_SIZE)
		return len;

	str +=
	    sprintf(str, "TCG version: %d.%d\nFirmware version: %d.%d\n",
		    (int) data[14], (int) data[15], (int) data[16],
		    (int) data[17]);

	return str - buf;
}

EXPORT_SYMBOL_GPL(tpm_show_caps);

ssize_t tpm_store_cancel(struct device * dev, const char *buf,
			 size_t count)
{
	struct tpm_chip *chip = dev_get_drvdata(dev);
	if (chip == NULL)
		return 0;

	chip->vendor->cancel(chip);
	return count;
}

EXPORT_SYMBOL_GPL(tpm_store_cancel);

/*
 * Device file system interface to the TPM
 */
int tpm_open(struct inode *inode, struct file *file)
{
	int rc = 0, minor = iminor(inode);
	struct tpm_chip *chip = NULL, *pos;

	spin_lock(&driver_lock);

	list_for_each_entry(pos, &tpm_chip_list, list) {
		if (pos->vendor->miscdev.minor == minor) {
			chip = pos;
			break;
		}
	}

	if (chip == NULL) {
		rc = -ENODEV;
		goto err_out;
	}

	if (chip->num_opens) {
		dev_dbg(chip->dev, "Another process owns this TPM\n");
		rc = -EBUSY;
		goto err_out;
	}

	chip->num_opens++;
	get_device(chip->dev);

	spin_unlock(&driver_lock);

	chip->data_buffer = kmalloc(TPM_BUFSIZE * sizeof(u8), GFP_KERNEL);
	if (chip->data_buffer == NULL) {
		chip->num_opens--;
		put_device(chip->dev);
		return -ENOMEM;
	}

	atomic_set(&chip->data_pending, 0);

	file->private_data = chip;
	return 0;

err_out:
	spin_unlock(&driver_lock);
	return rc;
}

EXPORT_SYMBOL_GPL(tpm_open);

int tpm_release(struct inode *inode, struct file *file)
{
	struct tpm_chip *chip = file->private_data;

	spin_lock(&driver_lock);
	file->private_data = NULL;
	chip->num_opens--;
	del_singleshot_timer_sync(&chip->user_read_timer);
	atomic_set(&chip->data_pending, 0);
	put_device(chip->dev);
	kfree(chip->data_buffer);
	spin_unlock(&driver_lock);
	return 0;
}

EXPORT_SYMBOL_GPL(tpm_release);

ssize_t tpm_write(struct file * file, const char __user * buf,
		  size_t size, loff_t * off)
{
	struct tpm_chip *chip = file->private_data;
	int in_size = size, out_size;

	/* cannot perform a write until the read has cleared
	   either via tpm_read or a user_read_timer timeout */
	while (atomic_read(&chip->data_pending) != 0)
		msleep(TPM_TIMEOUT);

	down(&chip->buffer_mutex);

	if (in_size > TPM_BUFSIZE)
		in_size = TPM_BUFSIZE;

	if (copy_from_user
	    (chip->data_buffer, (void __user *) buf, in_size)) {
		up(&chip->buffer_mutex);
		return -EFAULT;
	}

	/* atomic tpm command send and result receive */
	out_size = tpm_transmit(chip, chip->data_buffer, TPM_BUFSIZE);

	atomic_set(&chip->data_pending, out_size);
	up(&chip->buffer_mutex);

	/* Set a timeout by which the reader must come claim the result */
	mod_timer(&chip->user_read_timer, jiffies + (60 * HZ));

	return in_size;
}

EXPORT_SYMBOL_GPL(tpm_write);

ssize_t tpm_read(struct file * file, char __user * buf,
		 size_t size, loff_t * off)
{
	struct tpm_chip *chip = file->private_data;
	int ret_size;

	del_singleshot_timer_sync(&chip->user_read_timer);
	ret_size = atomic_read(&chip->data_pending);

	if (ret_size > 0) {	/* relay data */
		int position = atomic_read(&chip->data_position);

		if (size < ret_size)
			ret_size = size;

		down(&chip->buffer_mutex);

		if (copy_to_user((void __user *) buf,
				 &chip->data_buffer[position],
				 ret_size)) {
			ret_size = -EFAULT;
		} else {
		 	int pending = atomic_read(&chip->data_pending) - ret_size;
			atomic_set(&chip->data_pending,
			           pending);
			atomic_set(&chip->data_position,
			           position + ret_size);
		}
		up(&chip->buffer_mutex);
	}

	return ret_size;
}

EXPORT_SYMBOL_GPL(tpm_read);

void tpm_remove_hardware(struct device *dev)
{
	struct tpm_chip *chip = dev_get_drvdata(dev);
	int i;

	if (chip == NULL) {
		dev_err(dev, "No device data found\n");
		return;
	}

	spin_lock(&driver_lock);

	list_del(&chip->list);

	spin_unlock(&driver_lock);

	dev_set_drvdata(dev, NULL);
	misc_deregister(&chip->vendor->miscdev);

	for (i = 0; i < TPM_NUM_ATTR; i++)
		device_remove_file(dev, &chip->vendor->attr[i]);

	dev_mask[chip->dev_num / TPM_NUM_MASK_ENTRIES] &=
	    !(1 << (chip->dev_num % TPM_NUM_MASK_ENTRIES));

	kfree(chip);

	put_device(dev);
}

EXPORT_SYMBOL_GPL(tpm_remove_hardware);

static const u8 savestate[] = {
	0, 193,			/* TPM_TAG_RQU_COMMAND */
	0, 0, 0, 10,		/* blob length (in bytes) */
	0, 0, 0, 152		/* TPM_ORD_SaveState */
};

/*
 * We are about to suspend. Save the TPM state
 * so that it can be restored.
 */
int tpm_pm_suspend(struct pci_dev *pci_dev, u32 pm_state)
{
	struct tpm_chip *chip = pci_get_drvdata(pci_dev);
	if (chip == NULL)
		return -ENODEV;

	tpm_transmit(chip, savestate, sizeof(savestate));
	return 0;
}

EXPORT_SYMBOL_GPL(tpm_pm_suspend);

/*
 * Resume from a power safe. The BIOS already restored
 * the TPM state.
 */
int tpm_pm_resume(struct pci_dev *pci_dev)
{
	struct tpm_chip *chip = pci_get_drvdata(pci_dev);

	if (chip == NULL)
		return -ENODEV;

	return 0;
}

EXPORT_SYMBOL_GPL(tpm_pm_resume);

/*
 * Called from tpm_<specific>.c probe function only for devices
 * the driver has determined it should claim.  Prior to calling
 * this function the specific probe function has called pci_enable_device
 * upon errant exit from this function specific probe function should call
 * pci_disable_device
 */
int tpm_register_hardware_nopci(struct device *dev,
			        struct tpm_vendor_specific *entry)
{
	char devname[7];
	struct tpm_chip *chip;
	int i, j;

	/* Driver specific per-device data */
	chip = kmalloc(sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return -ENOMEM;

	memset(chip, 0, sizeof(struct tpm_chip));

	init_MUTEX(&chip->buffer_mutex);
	init_MUTEX(&chip->tpm_mutex);
	INIT_LIST_HEAD(&chip->list);

	init_timer(&chip->user_read_timer);
	chip->user_read_timer.function = user_reader_timeout;
	chip->user_read_timer.data = (unsigned long) chip;

	chip->vendor = entry;

	chip->dev_num = -1;

	for (i = 0; i < TPM_NUM_MASK_ENTRIES; i++)
		for (j = 0; j < 8 * sizeof(int); j++)
			if ((dev_mask[i] & (1 << j)) == 0) {
				chip->dev_num =
				    i * TPM_NUM_MASK_ENTRIES + j;
				dev_mask[i] |= 1 << j;
				goto dev_num_search_complete;
			}

dev_num_search_complete:
	if (chip->dev_num < 0) {
		dev_err(dev, "No available tpm device numbers\n");
		kfree(chip);
		return -ENODEV;
	} else if (chip->dev_num == 0)
		chip->vendor->miscdev.minor = TPM_MINOR;
	else
		chip->vendor->miscdev.minor = MISC_DYNAMIC_MINOR;

	snprintf(devname, sizeof(devname), "%s%d", "tpm", chip->dev_num);
	chip->vendor->miscdev.name = devname;

	chip->vendor->miscdev.dev = dev;
	chip->dev = get_device(dev);


	if (misc_register(&chip->vendor->miscdev)) {
		dev_err(chip->dev,
			"unable to misc_register %s, minor %d\n",
			chip->vendor->miscdev.name,
			chip->vendor->miscdev.minor);
		put_device(dev);
		kfree(chip);
		dev_mask[i] &= !(1 << j);
		return -ENODEV;
	}

	spin_lock(&driver_lock);

	dev_set_drvdata(dev, chip);

	list_add(&chip->list, &tpm_chip_list);

	spin_unlock(&driver_lock);

	for (i = 0; i < TPM_NUM_ATTR; i++)
		device_create_file(dev, &chip->vendor->attr[i]);

	return 0;
}

EXPORT_SYMBOL_GPL(tpm_register_hardware_nopci);

static int __init init_tpm(void)
{
	return 0;
}

static void __exit cleanup_tpm(void)
{

}

module_init(init_tpm);
module_exit(cleanup_tpm);

MODULE_AUTHOR("Leendert van Doorn (leendert@watson.ibm.com)");
MODULE_DESCRIPTION("TPM Driver");
MODULE_VERSION("2.0");
MODULE_LICENSE("GPL");
