/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Apple iBridge Driver
 *
 * Copyright (c) 2018 Ronald Tschalär
 */

#ifndef __LINUX_MFD_APPLE_IBRDIGE_H
#define __LINUX_MFD_APPLE_IBRDIGE_H

#include <linux/device.h>
#include <linux/hid.h>

#define PLAT_NAME_IB_TB		"apple-ib-tb"
#define PLAT_NAME_IB_ALS	"apple-ib-als"

struct appleib_device;

struct appleib_device_data {
	struct appleib_device *ib_dev;
	struct device *log_dev;
};

int appleib_register_hid_driver(struct appleib_device *ib_dev,
				struct hid_driver *driver, void *data);
int appleib_unregister_hid_driver(struct appleib_device *ib_dev,
				  struct hid_driver *driver);

void *appleib_get_drvdata(struct appleib_device *ib_dev,
			  struct hid_driver *driver);
bool appleib_needs_io_start(struct appleib_device *ib_dev,
			    struct hid_device *hdev);

struct hid_field *appleib_find_report_field(struct hid_report *report,
					    unsigned int field_usage);
struct hid_field *appleib_find_hid_field(struct hid_device *hdev,
					 unsigned int application,
					 unsigned int field_usage);

#endif
