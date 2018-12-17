/* oops_ubi.c -  Panic dump to ubi
 *
 * Copyright (C) 2014 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  * GNU General Public License for more details.
  *
  * You should have received a copy of the GNU General Public License
  * along with this program;
  */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/kmsg_dump.h>
#include <linux/time.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/mtd/ubi.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/scatterlist.h>
#include <linux/moduleparam.h>
#include <linux/errno.h>
#include <linux/hdreg.h>
#include <linux/kdev_t.h>
#include <linux/blkdev.h>
#include <linux/mutex.h>
#include <linux/string_helpers.h>
#include <linux/delay.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>



#define OOPS_UBI_DUMP_SIGNATURE "BRCM-OOPS-DUMP-UBI"

#define OOPS_UBI_DUMP_INVALID 0xFFFFFFFF
static char *dump_mark =
	"==============================";
static char *dump_start_str =
	"PREVIOUS_KERNEL_OOPS_DUMP_START";
static char *dump_end_str =
	"PREVIOUS_KERNEL_OOPS_DUMP_END";
static int actual_dumped_records;

static unsigned long ubi_dev_num = OOPS_UBI_DUMP_INVALID;
module_param(ubi_dev_num, ulong, 0400);
MODULE_PARM_DESC(ubi_dev_num,
		"ubi number to dump the panic");

static unsigned long ubi_vol = OOPS_UBI_DUMP_INVALID;
module_param(ubi_vol, ulong, 0400);
MODULE_PARM_DESC(ubi_vol,
		"ubi volume number to dump the panic");

static unsigned long ubi_vol_size = OOPS_UBI_DUMP_INVALID;
module_param(ubi_vol_size, ulong, 0400);
MODULE_PARM_DESC(ubi_vol_size,
		"ubi volume size to dump the panic");

static char *dump_file_path;
module_param(dump_file_path, charp, 0400);
MODULE_PARM_DESC(dump_file_path,
		"Dump the panic to file instead of console");

static char *dump_to_console;
module_param(dump_to_console, charp, 0400);
MODULE_PARM_DESC(dump_to_console,
		"Dump to console OR app can write script");


static unsigned long dump_offset_vol;
static struct platform_device *dummy;
static struct oops_ubi_platform_data *dummy_data;

struct oops_ubi_platform_data {
	unsigned long	ubi_dev_num;
	unsigned long	ubi_vol;
	unsigned long	ubi_vol_size;
	char			*dump_file_path;
	char			*dump_to_console;
};

static struct oops_ubi_context {
	struct kmsg_dumper dump;
	char			*buff;
	unsigned long	ubi_dev_num;
	unsigned long   ubi_vol;
	unsigned long   ubi_vol_size;
	unsigned long   ubi_min_io_size;
} oops_ubi_cxt;


static void oops_ubi_do_dump(struct kmsg_dumper *dumper,
				enum kmsg_dump_reason reason)
{
	struct oops_ubi_context *cxt =
	container_of(dumper, struct oops_ubi_context, dump);
	char *buff = cxt->buff;
	bool rc = true;
	size_t header_size, text_len = 0;
	int text_length = 0;
	struct ubi_volume_desc *ubi_fd;
	int err = 0;

	dump_offset_vol = cxt->ubi_min_io_size;

	if (actual_dumped_records > 0) {
		pr_err("oops_ubi_do_dump: Already dumped, Ignore this second call\n");
		return;
	}

	ubi_fd = ubi_open_volume(ubi_dev_num, ubi_vol, UBI_READWRITE);
	if (ubi_fd < 0) {
		pr_err("oops_ubi_do_dump: ubi_open_volume failed\n");
		return;
	}

	while (1) {
		if ((dump_offset_vol + cxt->ubi_min_io_size) >
						cxt->ubi_vol_size) {
			pr_err("oops_ubi_do_dump: can't write more than vol size\n");
			ubi_close_volume(ubi_fd);
			return;
		}

		/* Add Dump signature and
		 *block size before kernel log
		 */
		buff = (char *)cxt->buff;
		memset(buff, '\0', cxt->ubi_min_io_size);
		header_size = strlen(OOPS_UBI_DUMP_SIGNATURE)
							+ sizeof(int);
		buff += header_size;
		rc = kmsg_dump_get_buffer(dumper, false,
			buff, (cxt->ubi_min_io_size-header_size), &text_len);
		text_length = text_len;
		if (!rc) {
			pr_err("oops_ubi_do_dump: end of read from the kmsg dump\n");
			break;
		}
		buff = (char *)cxt->buff;
		memcpy(buff, OOPS_UBI_DUMP_SIGNATURE,
					strlen(OOPS_UBI_DUMP_SIGNATURE));
		buff = buff+strlen(OOPS_UBI_DUMP_SIGNATURE);
		memcpy(buff, &text_length, sizeof(int));
		pr_info("oops_ubi_do_dump: writing to UBI [%d] bytes\n",
								text_length);

		/*Write the data and update the offset for next dump call */
		err = ubi_leb_write(ubi_fd, 0, cxt->buff, dump_offset_vol,
						cxt->ubi_min_io_size);
		dump_offset_vol += cxt->ubi_min_io_size;
		actual_dumped_records++;
	}

	/* Write the first block with signature and actual dumped records */
	if (actual_dumped_records > 0) {
		memset(cxt->buff, '\0', cxt->ubi_min_io_size);
		memcpy(cxt->buff, OOPS_UBI_DUMP_SIGNATURE,
					strlen(OOPS_UBI_DUMP_SIGNATURE));
		memcpy(cxt->buff+strlen(OOPS_UBI_DUMP_SIGNATURE),
		&actual_dumped_records, sizeof(int));
		ubi_leb_write(ubi_fd, 0, cxt->buff, 0, cxt->ubi_min_io_size);
	}
	pr_err("oops_mmc_do_dump: total dumped records = %d\n",
		actual_dumped_records);
	ubi_close_volume(ubi_fd);
}

static int __init oops_ubi_probe(struct platform_device *pdev)
{
	struct oops_ubi_platform_data *pdata = pdev->dev.platform_data;
	struct oops_ubi_context *cxt = &oops_ubi_cxt;
	int err = 0;
	struct ubi_volume_desc *ubi_fd;
	struct ubi_device_info di;
	bool b_panic_dump = false, oem_own_script = false;
	int i = 0, j = 0;
	loff_t pos = 0;
	struct file *file = NULL;
	mm_segment_t old_fs;
	char *buf = NULL;
	int dumped_records = 0;
	int text_len = 0;
	char marker_string[200] = "";

	if (!pdata->ubi_vol_size) {
		pr_err("ubi_vol_size must be non-zero\n");
		return err;
	}

	ubi_dev_num = cxt->ubi_dev_num = pdata->ubi_dev_num;
	ubi_vol = cxt->ubi_vol = pdata->ubi_vol;
	ubi_vol_size = cxt->ubi_vol_size = pdata->ubi_vol_size;

    /*Get the ubi device info and its min IO size */
	err = ubi_get_device_info(ubi_dev_num, &di);
	if (err) {
		pr_err("oops_ubi_probe:  ubi_get_device_info failed\n");
		return err;
	}
	cxt->ubi_min_io_size = di.min_io_size;
	pr_err("dev_num=%ld, vol=%ld, vol_size=%ld, min_io_size=%ld\n",
		ubi_dev_num, ubi_vol, ubi_vol_size, cxt->ubi_min_io_size);

	/* Allocate min io size buffer to be used in do_dump */
	cxt->buff = vmalloc(cxt->ubi_min_io_size);
	if (cxt->buff == NULL)
		return -EINVAL;

	/* Register dump function with kmsger */
	cxt->dump.dump = oops_ubi_do_dump;
	err = kmsg_dump_register(&cxt->dump);
	if (err) {
		pr_err("registering kmsg dumper failed\n");
		err = -EINVAL;
		goto kmsg_register_failed;
	}

	buf = (char *)cxt->buff;

	ubi_fd = ubi_open_volume(ubi_dev_num, ubi_vol, UBI_READWRITE);
	if (ubi_fd < 0) {
		pr_err("oops_ubi_probe:  UBI open failed\n");
		err = -EINVAL;
		goto ubi_open_failed;
	}

	if (pdata->dump_file_path) {
		pr_info("oops_ubi_probe: dump_file_path = %s\n",
				pdata->dump_file_path);
		file = filp_open(pdata->dump_file_path,
				O_WRONLY|O_CREAT, 0644);
		if (IS_ERR(file)) {
			pr_err("oops_ubi_probe: filp_open failed, dump to console only\n");
			file = NULL;
		 } else {
		    old_fs = get_fs();
		    set_fs(get_ds());
		 }
	} else if ((pdata->dump_to_console) &&
			((!strcmp(pdata->dump_to_console, "n")) ||
			(!strcmp(pdata->dump_to_console, "no"))))
		pr_info("oops_ubi_probe: dump_to_console=no,OEM has own script\n");
	else
		pr_info("oops_ubi_probe: panic info will be dumped to console\n");


	/*Read the first block for signature*/
	err = ubi_leb_read(ubi_fd, 0, buf, 0, cxt->ubi_min_io_size, 0);
	if (err) {
		pr_err("oops_ubi_probe: ubi read failed\n");
		err = 0;
	}

	if (!strncmp(OOPS_UBI_DUMP_SIGNATURE,
			buf, strlen(OOPS_UBI_DUMP_SIGNATURE))) {
		b_panic_dump = true;
		sprintf(marker_string, "\n%s%s%s\n", dump_mark,
					dump_start_str, dump_mark);
		pr_err("%s", marker_string);
		if (file) {
			vfs_write(file, marker_string,
				strlen(marker_string), &pos);
			pos = pos+strlen(marker_string);

		}

		memcpy(&dumped_records,
			&buf[strlen(OOPS_UBI_DUMP_SIGNATURE)], sizeof(int));
		pr_err("oops_ubi_probe: found dumped records = %d\n",
				dumped_records);
		if (pdata->dump_file_path)
			pr_info("oops_ubi_probe: panics dumped to the file [%s]\n",
				pdata->dump_file_path);
		else if ((pdata->dump_to_console) &&
			((!strcmp(pdata->dump_to_console, "n")) ||
			(!strcmp(pdata->dump_to_console, "no")))) {
				pr_info("oops_ubi_probe:OEM has own script to read!\n");
				err = 0;
				oem_own_script = true;
				goto ubi_op_exit;
		}

		if (dumped_records >=
			(cxt->ubi_vol_size/cxt->ubi_min_io_size)) {
			pr_info("oops_ubi_probe: Invalid dumped_records[%d]\n",
					dumped_records);
			dumped_records = 0;
		}
	} else
		pr_info("oops_ubi_probe: There was no panic in earlier run\n");


	for (i = dumped_records; i > 0; i--) {
		buf = (char *)cxt->buff;
		err = ubi_leb_read(ubi_fd, 0, buf, (i*cxt->ubi_min_io_size),
					cxt->ubi_min_io_size, 0);
		if (err) {
			pr_err("oops_ubi_probe:  ubi read failed\n");
			err = 0;
		}

		if (strncmp(OOPS_UBI_DUMP_SIGNATURE, buf,
					strlen(OOPS_UBI_DUMP_SIGNATURE))) {
			pr_err("DUMP_SIGNATURE didn't match for record[%d]\n",
					i);
			continue;
		}
		memcpy(&text_len, &buf[strlen(OOPS_UBI_DUMP_SIGNATURE)],
					sizeof(int));
		buf = buf+strlen(OOPS_UBI_DUMP_SIGNATURE)+sizeof(int);

		if ((text_len == 0) || (text_len > cxt->ubi_min_io_size)) {
			pr_info("oops_ubi_probe:Invalid text length[%d]\n",
					text_len);
			break;
		}
		pr_info("oops_ubi_probe: printing text length = %d\n",
							text_len);

		if (file) {
			vfs_write(file, buf, text_len, &pos);
			pos = pos+text_len;
		} else {
			for (j = 0; j < text_len; j++)
				pr_err("%c", buf[j]);
		}
	}


ubi_op_exit:
	if (b_panic_dump) {
		/* Erase the data for next panic dump */
		if (!oem_own_script) {
			if (ubi_leb_erase(ubi_fd, 0))
				pr_err("oops_ubi_probe:  ubi erase failed\n");
		}

		sprintf(marker_string, "\n%s%s%s\n", dump_mark,
						dump_end_str, dump_mark);
		pr_err("%s", marker_string);
		if (file) {
			vfs_write(file, marker_string,
				strlen(marker_string), &pos);
			pos = pos+strlen(marker_string);
		}

	}

	/* Close the ubi volume */
	ubi_close_volume(ubi_fd);
	if (file) {
		filp_close(file, NULL);
		set_fs(old_fs);
		file = NULL;
	}
	pr_err("oops_ubi_probe: Done. err=%d\n", err);
	return err;

ubi_open_failed:
	pr_err("oops_ubi_probe: failed. err=%d\n", err);
	if (kmsg_dump_unregister(&cxt->dump) < 0)
		pr_err("oops_ubi_probe: could not unregister kmsg_dumper\n");

kmsg_register_failed:
	vfree(cxt->buff);
	cxt->buff = NULL;
	return err;

}

static int __exit oops_ubi_remove(struct platform_device *pdev)
{
	struct oops_ubi_context *cxt = &oops_ubi_cxt;

	pr_err("oops_ubi_remove: Enter\n");

	if (kmsg_dump_unregister(&cxt->dump) < 0)
		pr_err("could not unregister kmsg_dumper\n");

	if (cxt->buff)  {
		vfree(cxt->buff);
	cxt->buff = NULL;
	}
	return 0;
}

static struct platform_driver oops_ubi_driver = {
	.remove		= __exit_p(oops_ubi_remove),
	.probe		= oops_ubi_probe,
	.driver		= {
		.name	= "oops_ubi",
		.owner	= THIS_MODULE,
	},
};

static int __init oops_ubi_init(void)
{
	int ret = 0;

	pr_err("oops_ubi_init: Enter\n");

	ret = platform_driver_probe(&oops_ubi_driver, oops_ubi_probe);
	if (ret == -ENODEV) {
		/*
		* If we didn't find a platform device, we use module parameters
		* building platform data on the fly.
		*/
		pr_info("platform device not found, using module parameters\n");
		dummy_data = kzalloc(sizeof(struct oops_ubi_platform_data),
		GFP_KERNEL);

		if (!dummy_data)
			return -ENOMEM;

		if (ubi_dev_num == OOPS_UBI_DUMP_INVALID ||
			ubi_vol == OOPS_UBI_DUMP_INVALID ||
			ubi_vol_size == 0 ||
			ubi_vol_size == OOPS_UBI_DUMP_INVALID) {
				pr_err("oops_ubi_init : Invalid module params\n");
				ret = -EINVAL;
				goto error;
		}

		dummy_data->ubi_dev_num = ubi_dev_num;
		dummy_data->ubi_vol = ubi_vol;
		dummy_data->ubi_vol_size = ubi_vol_size;
		dummy_data->dump_file_path = dump_file_path;
		dummy_data->dump_to_console = dump_to_console;

		dummy = platform_create_bundle(&oops_ubi_driver, oops_ubi_probe,
			NULL, 0, dummy_data,
			sizeof(struct oops_ubi_platform_data));

		if (IS_ERR(dummy)) {
			pr_err("oops_ubi_init: platform create failed\n");
			ret = PTR_ERR(dummy);
			goto error;
		}
	}
	return 0;

error:
	pr_err("oops_ubi_init : failed.\n");
	kfree(dummy_data);
	dummy_data = NULL;
	return ret;
}

static void __exit oops_ubi_exit(void)
{
	pr_info("oops_ubi_exit\n");

	kfree(dummy_data);
	dummy_data = NULL;
	platform_device_unregister(dummy);
	platform_driver_unregister(&oops_ubi_driver);
	dummy = NULL;
}

module_init(oops_ubi_init);
module_exit(oops_ubi_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BRCM");
MODULE_DESCRIPTION("Oops to UBI/Panic logger/driver");
