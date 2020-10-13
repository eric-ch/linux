/*
 * Copyright (C) 2017 Apertus Solutions, LLC
 *
 * Authors:
 *      Daniel P. Smith <dpsmith@apertussolutions.com>
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/security.h>
#include <linux/tpm.h>

#include <xen/xen.h>
#include <asm/xen/hypercall.h>

#define FS_LOG_ENTRY 0
#define FS_DIR_ENTRY 1 /* must be last */
#define FS_ENTRIES 2

static struct txt_op txt_log;
static struct dentry *fs_entries[FS_ENTRIES];


#define TPM_LOG_BLOCK_SIZE 1024

static void *tpm_evtlog_start(struct seq_file *m, loff_t *pos)
{
	struct txt_op *log = m->private;
	void *addr = log->buffer;

	addr += *pos * TPM_LOG_BLOCK_SIZE;

	if (addr > log->buffer + log->size)
		return NULL;

	return addr;
}

static void *tpm_evtlog_next(struct seq_file *m, void *v, loff_t *pos)
{
	size_t size = 0;
	struct txt_op *log = m->private;
	void *addr = v;

	size = (log->buffer + log->size) - addr;
	size = size > TPM_LOG_BLOCK_SIZE ? TPM_LOG_BLOCK_SIZE : size;

	if ((size == 0) ||
	    ((addr + size) > (log->buffer + log->size)))
		return NULL;

	addr += size;
	(*pos)++;

	return addr;
}

static void tpm_evtlog_stop(struct seq_file *m, void *v)
{
}

static int tpm_evtlog_show(struct seq_file *m, void *v)
{
	size_t size;
	struct txt_op *log = m->private;
	void *addr = v;

	size = ((log->buffer + log->size) - addr) > TPM_LOG_BLOCK_SIZE ?
		TPM_LOG_BLOCK_SIZE : (log->buffer + log->size) - addr;

	if ((size != 0) &&
	    ((addr + size) <= (log->buffer + log->size)))
		seq_write(m, addr, size);

	return 0;
}

const struct seq_operations tpm_evtlog_seqops = {
	.start = tpm_evtlog_start,
	.next = tpm_evtlog_next,
	.stop = tpm_evtlog_stop,
	.show = tpm_evtlog_show,
};

static int tpm_evtlog_open(struct inode *inode, struct file *file)
{
	int err;
	struct seq_file *seq;

	err = seq_open(file, &tpm_evtlog_seqops);
	if (!err) {
		seq = file->private_data;
		seq->private = &txt_log;
	}

	return err;
}

static const struct file_operations tpm_evtlog_ops = {
        .open = tpm_evtlog_open,
        .read = seq_read,
        .llseek = seq_lseek,
        .release = seq_release,
};


static long expose_evtlog(const char *name)
{
	long ret = 0;
	char *filename;

	fs_entries[FS_DIR_ENTRY] = securityfs_create_dir(name, NULL);
	if (IS_ERR(fs_entries[FS_DIR_ENTRY])) {
		ret = PTR_ERR(fs_entries[FS_DIR_ENTRY]);
		goto out;
	}

	switch (txt_log.format) {
		case TXTOP_EVTLOG_FORMAT_TCG_12:
			filename = "tpm12_binary_evtlog";
			break;
		case TXTOP_EVTLOG_FORMAT_LEGACY_20:
			filename = "tpm20_binary_evtlog_legacy";
			break;
		case TXTOP_EVTLOG_FORMAT_TCG_20:
			filename = "tpm20_binary_evtlog_tcg";
			break;
		default:
			printk(KERN_ERR "Incompatible event-log format: %x\n", txt_log.format);
			ret = EINVAL;
			goto out_dir;
	}

	fs_entries[FS_LOG_ENTRY] =
	    securityfs_create_file(filename,
				   S_IRUSR | S_IRGRP,
				   fs_entries[FS_DIR_ENTRY], NULL,
				   &tpm_evtlog_ops);
	if (IS_ERR(fs_entries[FS_LOG_ENTRY])) {
		ret = PTR_ERR(fs_entries[FS_LOG_ENTRY]);
		goto out_dir;
	}

	return 0;

out_dir:
	securityfs_remove(fs_entries[FS_DIR_ENTRY]);
out:
	return ret;
}

void teardown_evtlog(void)
{
	int i;

	for (i = 0; i < FS_ENTRIES; i++)
		securityfs_remove(fs_entries[i]);
}

static int __init txt_init(void)
{
	int err;

	if (!xen_domain())
		return -ENODEV;

	txt_log.size = 0;
	txt_log.buffer = NULL;
	txt_log.format = 0;
	if ((err = HYPERVISOR_txt_op(TXTOP_GET, &txt_log)) != 0)
		return err;

	if (!txt_log.size)
		return -ENODEV;

	txt_log.buffer = kmalloc(txt_log.size, GFP_KERNEL);
	if (txt_log.buffer == NULL)
		return -ENOMEM;

	if ((err = HYPERVISOR_txt_op(TXTOP_GET, &txt_log)) != 0)
		goto error;

	if ((err = expose_evtlog("txt")) != 0)
		goto error;

	return 0;

error:
	kfree(txt_log.buffer);
	return err;
}

static void __exit txt_exit(void)
{
	teardown_evtlog();

	if (txt_log.buffer)
		kfree(txt_log.buffer);
}

module_init(txt_init);
module_exit(txt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Daniel P. Smith <dpsmith@apertussolutions.com>");
MODULE_DESCRIPTION("TXT TPM Event log");
