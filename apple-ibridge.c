// SPDX-License-Identifier: GPL-2.0
/*
 * Apple iBridge Driver
 *
 * Copyright (c) 2018 Ronald Tschalär
 */

/**
 * DOC: Overview
 *
 * MacBookPro models with a Touch Bar (13,[23] and 14,[23]) have an Apple
 * iBridge chip (also known as T1 chip) which exposes the touch bar,
 * built-in webcam (iSight), ambient light sensor, and Secure Enclave
 * Processor (SEP) for TouchID. It shows up in the system as a USB device
 * with 3 configurations: 'Default iBridge Interfaces', 'Default iBridge
 * Interfaces(OS X)', and 'Default iBridge Interfaces(Recovery)'. While
 * the second one is used by MacOS to provide the fancy touch bar
 * functionality with custom buttons etc, this driver just uses the first.
 *
 * In the first (default after boot) configuration, 4 usb interfaces are
 * exposed: 2 related to the webcam, and 2 USB HID interfaces representing
 * the touch bar and the ambient light sensor (and possibly the SEP,
 * though at this point in time nothing is known about that). The webcam
 * interfaces are already handled by the uvcvideo driver; furthermore, the
 * handling of the input reports when "keys" on the touch bar are pressed
 * is already handled properly by the generic USB HID core. This leaves
 * the management of the touch bar modes (e.g. switching between function
 * and special keys when the FN key is pressed), the touch bar display
 * (dimming and turning off), the key-remapping when the FN key is
 * pressed, and handling of the light sensor.
 *
 * This driver is implemented as an MFD driver, with the touch bar and ALS
 * functions implemented by appropriate subdrivers (mfd cells). Because
 * both those are basically hid drivers, but the current kernel driver
 * structure does not allow more than one driver per device, this driver
 * implements a demuxer for hid drivers: it registers itself as a hid
 * driver with the core, and in turn it lets the subdrivers register
 * themselves as hid drivers with this driver; the callbacks from the core
 * are then forwarded to the subdrivers.
 *
 * Lastly, this driver also takes care of the power-management for the
 * iBridge when suspending and resuming.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/list.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/srcu.h>
#include <linux/usb.h>
#include <linux/version.h>

#include <asm/barrier.h>

#include "apple-ibridge.h"

#ifdef UPSTREAM
#include "../hid/usbhid/usbhid.h"
#else
#define	hid_to_usb_dev(hid_dev) \
	to_usb_device((hid_dev)->dev.parent->parent)
#endif

#define USB_ID_VENDOR_APPLE	0x05ac
#define USB_ID_PRODUCT_IBRIDGE	0x8600

#define APPLETB_BASIC_CONFIG	1

#define	LOG_DEV(ib_dev)		(&(ib_dev)->acpi_dev->dev)

static const struct mfd_cell appleib_subdevs[] = {
	{ .name = PLAT_NAME_IB_TB },
	{ .name = PLAT_NAME_IB_ALS },
};

static const struct hid_device_id appleib_hid_ids[] = {
	{ HID_USB_DEVICE(USB_ID_VENDOR_APPLE, USB_ID_PRODUCT_IBRIDGE) },
	{ },
};

struct appleib_device {
	struct acpi_device		*acpi_dev;
	acpi_handle			asoc_socw;
	struct list_head		hid_drivers;
	struct list_head		hid_devices;
	struct mfd_cell			subdevs[ARRAY_SIZE(appleib_subdevs)];
	/* protect updates to all lists */
	struct mutex			update_lock;
	struct srcu_struct		lists_srcu;
	struct hid_device		*needs_io_start;
	struct appleib_device_data	dev_data;
	struct hid_driver		ib_driver;
	struct hid_device_id		ib_dev_ids[ARRAY_SIZE(appleib_hid_ids)];
};

struct appleib_hid_drv_info {
	struct list_head	entry;
	struct hid_driver	*driver;
	void			*driver_data;
};

struct appleib_hid_dev_info {
	struct list_head		entry;
	struct list_head		drivers;
	struct hid_device		*device;
	const struct hid_device_id	*device_id;
	bool				started;
};

static void appleib_remove_driver(struct appleib_device *ib_dev,
				  struct appleib_hid_drv_info *drv_info,
				  struct appleib_hid_dev_info *dev_info)
{
	list_del_rcu(&drv_info->entry);
	synchronize_srcu(&ib_dev->lists_srcu);

	if (drv_info->driver->remove)
		drv_info->driver->remove(dev_info->device);

	kfree(drv_info);
}

static int appleib_probe_driver(struct appleib_hid_drv_info *drv_info,
				struct appleib_hid_dev_info *dev_info)
{
	struct appleib_hid_drv_info *d;
	int rc = 0;

	if (drv_info->driver->probe)
		rc = drv_info->driver->probe(dev_info->device,
					     dev_info->device_id);
	if (rc)
		return rc;

	d = kmemdup(drv_info, sizeof(*drv_info), GFP_KERNEL);
	if (!d) {
		if (drv_info->driver->remove)
			drv_info->driver->remove(dev_info->device);
		return -ENOMEM;
	}

	list_add_tail_rcu(&d->entry, &dev_info->drivers);
	return 0;
}

static void appleib_remove_driver_attachments(struct appleib_device *ib_dev,
					struct appleib_hid_dev_info *dev_info,
					struct hid_driver *driver)
{
	struct appleib_hid_drv_info *drv_info;
	struct appleib_hid_drv_info *tmp;

	list_for_each_entry_safe(drv_info, tmp, &dev_info->drivers, entry) {
		if (!driver || drv_info->driver == driver)
			appleib_remove_driver(ib_dev, drv_info, dev_info);
	}
}

/*
 * Find all devices that are attached to this driver and detach them.
 *
 * Note: this must be run with update_lock held.
 */
static void appleib_detach_devices(struct appleib_device *ib_dev,
				   struct hid_driver *driver)
{
	struct appleib_hid_dev_info *dev_info;

	list_for_each_entry(dev_info, &ib_dev->hid_devices, entry)
		appleib_remove_driver_attachments(ib_dev, dev_info, driver);
}

/*
 * Note: this must be run with update_lock held.
 */
static void appleib_remove_device(struct appleib_device *ib_dev,
				  struct appleib_hid_dev_info *dev_info)
{
	list_del_rcu(&dev_info->entry);
	synchronize_srcu(&ib_dev->lists_srcu);

	appleib_remove_driver_attachments(ib_dev, dev_info, NULL);

	kfree(dev_info);
}

void appleib_detach_and_free_hid_driver(struct appleib_device *ib_dev,
					struct appleib_hid_drv_info *drv_info)
{
	appleib_detach_devices(ib_dev, drv_info->driver);
	list_del_rcu(&drv_info->entry);
	synchronize_srcu(&ib_dev->lists_srcu);
	kfree(drv_info);
}

/**
 * appleib_unregister_hid_driver() - Unregister a previously registered HID
 * driver from us.
 * @ib_dev: the appleib_device from which to unregister the driver
 * @driver: the driver to unregister
 *
 * Return: 0 on success, or -ENOENT if the driver isn't currently registered.
 */
int appleib_unregister_hid_driver(struct appleib_device *ib_dev,
				  struct hid_driver *driver)
{
	struct appleib_hid_drv_info *drv_info;

	mutex_lock(&ib_dev->update_lock);

	list_for_each_entry(drv_info, &ib_dev->hid_drivers, entry) {
		if (drv_info->driver == driver) {
			appleib_detach_and_free_hid_driver(ib_dev, drv_info);

			mutex_unlock(&ib_dev->update_lock);

			dev_dbg(LOG_DEV(ib_dev), "unregistered driver '%s'\n",
				driver->name);
			return 0;
		}
	}

	mutex_unlock(&ib_dev->update_lock);

	dev_err(LOG_DEV(ib_dev),
		"Error unregistering hid driver '%s': driver not registered\n",
		driver->name);

	return -ENOENT;
}
EXPORT_SYMBOL_GPL(appleib_unregister_hid_driver);

static int appleib_start_hid_events(struct appleib_hid_dev_info *dev_info)
{
	struct hid_device *hdev = dev_info->device;
	int rc;

	rc = hid_connect(hdev, HID_CONNECT_DEFAULT);
	if (rc) {
		hid_err(hdev, "ib: hid connect failed (%d)\n", rc);
		return rc;
	}

	rc = hid_hw_open(hdev);
	if (rc) {
		hid_err(hdev, "ib: failed to open hid: %d\n", rc);
		hid_disconnect(hdev);
	}

	if (!rc)
		dev_info->started = true;

	return rc;
}

static void appleib_stop_hid_events(struct appleib_hid_dev_info *dev_info)
{
	if (dev_info->started) {
		hid_hw_close(dev_info->device);
		hid_disconnect(dev_info->device);
		dev_info->started = false;
	}
}

/**
 * appleib_register_hid_driver() - Register a HID driver with us.
 * @ib_dev: the appleib_device with which to register the driver
 * @driver: the driver to register
 * @data: the driver-data to associate with the driver; this is available
 *        from appleib_get_drvdata().
 *
 * Return: 0 on success or -errno
 */
int appleib_register_hid_driver(struct appleib_device *ib_dev,
				struct hid_driver *driver, void *data)
{
	struct appleib_hid_drv_info *drv_info;
	struct appleib_hid_dev_info *dev_info;
	struct appleib_hid_dev_info *tmp;
	int rc;

	if (!driver->probe)
		return -EINVAL;

	drv_info = kzalloc(sizeof(*drv_info), GFP_KERNEL);
	if (!drv_info)
		return -ENOMEM;

	drv_info->driver = driver;
	drv_info->driver_data = data;

	mutex_lock(&ib_dev->update_lock);

	list_add_tail_rcu(&drv_info->entry, &ib_dev->hid_drivers);

	list_for_each_entry_safe(dev_info, tmp, &ib_dev->hid_devices, entry) {
		appleib_stop_hid_events(dev_info);

		appleib_probe_driver(drv_info, dev_info);

		rc = appleib_start_hid_events(dev_info);
		if (rc)
			appleib_remove_device(ib_dev, dev_info);
	}

	mutex_unlock(&ib_dev->update_lock);

	dev_dbg(LOG_DEV(ib_dev), "registered driver '%s'\n", driver->name);

	return 0;
}
EXPORT_SYMBOL_GPL(appleib_register_hid_driver);

/**
 * appleib_get_drvdata() - Get the driver-specific data associated with the
 * given HID driver.
 * @ib_dev: the appleib_device with which the driver was registered
 * @driver: the driver for which to get the data
 *
 * Gets the driver-specific data for a registered driver that provided in the
 * appleib_register_hid_driver() call.
 *
 * Returns: the driver data, or NULL if the driver is not registered.
 */
void *appleib_get_drvdata(struct appleib_device *ib_dev,
			  struct hid_driver *driver)
{
	struct appleib_hid_drv_info *drv_info;
	void *drv_data = NULL;
	int idx;

	idx = srcu_read_lock(&ib_dev->lists_srcu);

	list_for_each_entry_rcu(drv_info, &ib_dev->hid_drivers, entry) {
		if (drv_info->driver == driver) {
			drv_data = drv_info->driver_data;
			break;
		}
	}

	srcu_read_unlock(&ib_dev->lists_srcu, idx);

	return drv_data;
}
EXPORT_SYMBOL_GPL(appleib_get_drvdata);

/**
 * appleib_forward_int_op() - Forward a hid-driver callback to all registered
 * sub-drivers.
 * @hdev the hid-device
 * @forward a function that calls the callback on the given driver
 * @args arguments for the forward function
 *
 * This is for callbacks that return a status as an int.
 *
 * Returns: 0 on success, or the first error returned by the @forward function.
 */
static int appleib_forward_int_op(struct hid_device *hdev,
				  int (*forward)(struct appleib_hid_drv_info *,
						 struct hid_device *, void *),
				  void *args)
{
	struct appleib_device *ib_dev = hid_get_drvdata(hdev);
	struct appleib_hid_dev_info *dev_info;
	struct appleib_hid_drv_info *drv_info;
	int idx;
	int rc = 0;

	idx = srcu_read_lock(&ib_dev->lists_srcu);

	list_for_each_entry_rcu(dev_info, &ib_dev->hid_devices, entry) {
		if (dev_info->device != hdev)
			continue;

		list_for_each_entry_rcu(drv_info, &dev_info->drivers, entry) {
			rc = forward(drv_info, hdev, args);
			if (rc)
				break;
		}

		break;
	}

	srcu_read_unlock(&ib_dev->lists_srcu, idx);

	return rc;
}

struct appleib_hid_event_args {
	struct hid_field *field;
	struct hid_usage *usage;
	__s32 value;
};

static int appleib_hid_event_fwd(struct appleib_hid_drv_info *drv_info,
				 struct hid_device *hdev, void *args)
{
	struct appleib_hid_event_args *evt_args = args;
	int rc = 0;

	if (drv_info->driver->event)
		rc = drv_info->driver->event(hdev, evt_args->field,
					     evt_args->usage, evt_args->value);

	return rc;
}

static int appleib_hid_event(struct hid_device *hdev, struct hid_field *field,
			     struct hid_usage *usage, __s32 value)
{
	struct appleib_hid_event_args args = {
		.field = field,
		.usage = usage,
		.value = value,
	};

	return appleib_forward_int_op(hdev, appleib_hid_event_fwd, &args);
}

static __u8 *appleib_report_fixup(struct hid_device *hdev, __u8 *rdesc,
				  unsigned int *rsize)
{
	/* Some fields have a size of 64 bits, which according to HID 1.11
	 * Section 8.4 is not valid ("An item field cannot span more than 4
	 * bytes in a report"). Furthermore, hid_field_extract() complains
	 * when encountering such a field. So turn them into two 32-bit fields
	 * instead.
	 */

	if (*rsize == 634 &&
	    /* Usage Page 0xff12 (vendor defined) */
	    rdesc[212] == 0x06 && rdesc[213] == 0x12 && rdesc[214] == 0xff &&
	    /* Usage 0x51 */
	    rdesc[416] == 0x09 && rdesc[417] == 0x51 &&
	    /* report size 64 */
	    rdesc[432] == 0x75 && rdesc[433] == 64 &&
	    /* report count 1 */
	    rdesc[434] == 0x95 && rdesc[435] == 1) {
		rdesc[433] = 32;
		rdesc[435] = 2;
		hid_dbg(hdev, "Fixed up first 64-bit field\n");
	}

	if (*rsize == 634 &&
	    /* Usage Page 0xff12 (vendor defined) */
	    rdesc[212] == 0x06 && rdesc[213] == 0x12 && rdesc[214] == 0xff &&
	    /* Usage 0x51 */
	    rdesc[611] == 0x09 && rdesc[612] == 0x51 &&
	    /* report size 64 */
	    rdesc[627] == 0x75 && rdesc[628] == 64 &&
	    /* report count 1 */
	    rdesc[629] == 0x95 && rdesc[630] == 1) {
		rdesc[628] = 32;
		rdesc[630] = 2;
		hid_dbg(hdev, "Fixed up second 64-bit field\n");
	}

	return rdesc;
}

static int appleib_input_configured_fwd(struct appleib_hid_drv_info *drv_info,
					struct hid_device *hdev, void *args)
{
	struct hid_input *inp = args;
	int rc = 0;

	if (drv_info->driver->input_configured)
		rc = drv_info->driver->input_configured(hdev, inp);

	return rc;
}

static int appleib_input_configured(struct hid_device *hdev,
				    struct hid_input *hidinput)
{
	return appleib_forward_int_op(hdev, appleib_input_configured_fwd,
				      hidinput);
}

#ifdef CONFIG_PM
static int appleib_hid_suspend_fwd(struct appleib_hid_drv_info *drv_info,
				   struct hid_device *hdev, void *args)
{
	int rc = 0;

	if (drv_info->driver->suspend)
		rc = drv_info->driver->suspend(hdev, *(pm_message_t *)args);

	return rc;
}

static int appleib_hid_suspend(struct hid_device *hdev, pm_message_t message)
{
	return appleib_forward_int_op(hdev, appleib_hid_suspend_fwd, &message);
}

static int appleib_hid_resume_fwd(struct appleib_hid_drv_info *drv_info,
				  struct hid_device *hdev, void *args)
{
	int rc = 0;

	if (drv_info->driver->resume)
		rc = drv_info->driver->resume(hdev);

	return rc;
}

static int appleib_hid_resume(struct hid_device *hdev)
{
	return appleib_forward_int_op(hdev, appleib_hid_resume_fwd, NULL);
}

static int appleib_hid_reset_resume_fwd(struct appleib_hid_drv_info *drv_info,
					struct hid_device *hdev, void *args)
{
	int rc = 0;

	if (drv_info->driver->reset_resume)
		rc = drv_info->driver->reset_resume(hdev);

	return rc;
}

static int appleib_hid_reset_resume(struct hid_device *hdev)
{
	return appleib_forward_int_op(hdev, appleib_hid_reset_resume_fwd, NULL);
}
#endif /* CONFIG_PM */

/**
 * appleib_find_report_field() - Find the field in the report with the given
 * usage.
 * @report: the report to search
 * @field_usage: the usage of the field to search for
 *
 * Returns: the hid field if found, or NULL if none found.
 */
struct hid_field *appleib_find_report_field(struct hid_report *report,
					    unsigned int field_usage)
{
	int f, u;

	for (f = 0; f < report->maxfield; f++) {
		struct hid_field *field = report->field[f];

		if (field->logical == field_usage)
			return field;

		for (u = 0; u < field->maxusage; u++) {
			if (field->usage[u].hid == field_usage)
				return field;
		}
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(appleib_find_report_field);

/**
 * appleib_find_hid_field() - Search all the reports of the device for the
 * field with the given usage.
 * @hdev: the device whose reports to search
 * @application: the usage of application collection that the field must
 *               belong to
 * @field_usage: the usage of the field to search for
 *
 * Returns: the hid field if found, or NULL if none found.
 */
struct hid_field *appleib_find_hid_field(struct hid_device *hdev,
					 unsigned int application,
					 unsigned int field_usage)
{
	static const int report_types[] = { HID_INPUT_REPORT, HID_OUTPUT_REPORT,
					    HID_FEATURE_REPORT };
	struct hid_report *report;
	struct hid_field *field;
	int t;

	for (t = 0; t < ARRAY_SIZE(report_types); t++) {
		struct list_head *report_list =
			    &hdev->report_enum[report_types[t]].report_list;
		list_for_each_entry(report, report_list, list) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0)
			if (report->application != application)
				continue;
#endif

			field = appleib_find_report_field(report, field_usage);
			if (field)
				return field;
		}
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(appleib_find_hid_field);

/**
 * appleib_needs_io_start() - Indicate whether the given hid device needs an
 * io-start.
 * @ib_dev: the appleib_device
 * @hdev: the hid_device
 *
 * Returns: true if the given hid device needs an io-start in order for
 * incoming packets to be delivered to the driver, false otherwise.
 */
bool appleib_needs_io_start(struct appleib_device *ib_dev,
			    struct hid_device *hdev)
{
	/* this may be called from multiple tasks for different hdev's */
	return smp_load_acquire(&ib_dev->needs_io_start) == hdev;
}
EXPORT_SYMBOL_GPL(appleib_needs_io_start);

static struct appleib_hid_dev_info *
appleib_add_device(struct appleib_device *ib_dev, struct hid_device *hdev,
		   const struct hid_device_id *id)
{
	struct appleib_hid_dev_info *dev_info;
	struct appleib_hid_drv_info *drv_info;

	dev_info = kzalloc(sizeof(*dev_info), GFP_KERNEL);
	if (!dev_info)
		return NULL;

	INIT_LIST_HEAD(&dev_info->drivers);
	dev_info->device = hdev;
	dev_info->device_id = id;

	mutex_lock(&ib_dev->update_lock);

	/*
	 * Indicate to sub-drivers that we're in a probe() call and therefore
	 * hid_device_io_start() needs to be explicitly called if the
	 * sub-driver's probe callback wants to receive incoming packets.
	 *
	 * This may be read concurrently from another task for another hdev.
	 */
	smp_store_release(&ib_dev->needs_io_start, hdev);

	list_for_each_entry(drv_info, &ib_dev->hid_drivers, entry) {
		appleib_probe_driver(drv_info, dev_info);
	}

	/* this may be read concurrently from another task for another hdev */
	smp_store_release(&ib_dev->needs_io_start, NULL);

	list_add_tail_rcu(&dev_info->entry, &ib_dev->hid_devices);

	mutex_unlock(&ib_dev->update_lock);

	return dev_info;
}

static int appleib_hid_probe(struct hid_device *hdev,
			     const struct hid_device_id *id)
{
	struct appleib_device *ib_dev;
	struct appleib_hid_dev_info *dev_info;
	struct usb_device *udev;
	int rc;

	/* check and set usb config first */
	udev = hid_to_usb_dev(hdev);

	if (udev->actconfig->desc.bConfigurationValue != APPLETB_BASIC_CONFIG) {
		rc = usb_driver_set_configuration(udev, APPLETB_BASIC_CONFIG);
		return rc ? rc : -ENODEV;
	}

	ib_dev = (void *)id->driver_data;
	hid_set_drvdata(hdev, ib_dev);

	rc = hid_parse(hdev);
	if (rc) {
		hid_err(hdev, "ib: hid parse failed (%d)\n", rc);
		goto error;
	}

	/* alloc bufs etc so probe's can send requests; but connect later */
	rc = hid_hw_start(hdev, 0);
	if (rc) {
		hid_err(hdev, "ib: hw start failed (%d)\n", rc);
		goto error;
	}

	dev_info = appleib_add_device(ib_dev, hdev, id);
	if (!dev_info) {
		rc = -ENOMEM;
		goto stop_hw;
	}

	rc = appleib_start_hid_events(dev_info);
	if (rc)
		goto remove_dev;

	return 0;

remove_dev:
	mutex_lock(&ib_dev->update_lock);
	appleib_remove_device(ib_dev, dev_info);
	mutex_unlock(&ib_dev->update_lock);
stop_hw:
	hid_hw_stop(hdev);
error:
	return rc;
}

static void appleib_hid_remove(struct hid_device *hdev)
{
	struct appleib_device *ib_dev = hid_get_drvdata(hdev);
	struct appleib_hid_dev_info *dev_info;

	mutex_lock(&ib_dev->update_lock);

	/*
	 * Indicate to sub-drivers that we're in a remove() call and therefore
	 * hid_device_io_start() needs to be explicitly called if the
	 * sub-driver's remove callback wants to receive incoming packets.
	 *
	 * This may be read concurrently from another task for another hdev.
	 */
	smp_store_release(&ib_dev->needs_io_start, hdev);

	list_for_each_entry(dev_info, &ib_dev->hid_devices, entry) {
		if (dev_info->device == hdev) {
			appleib_stop_hid_events(dev_info);
			appleib_remove_device(ib_dev, dev_info);
			break;
		}
	}

	/* this may be read concurrently from another task for another hdev */
	smp_store_release(&ib_dev->needs_io_start, NULL);

	mutex_unlock(&ib_dev->update_lock);

	hid_hw_stop(hdev);
}

static const struct hid_driver appleib_hid_driver = {
	.name = "apple-ibridge-hid",
	.id_table = appleib_hid_ids,
	.probe = appleib_hid_probe,
	.remove = appleib_hid_remove,
	.event = appleib_hid_event,
	.report_fixup = appleib_report_fixup,
	.input_configured = appleib_input_configured,
#ifdef CONFIG_PM
	.suspend = appleib_hid_suspend,
	.resume = appleib_hid_resume,
	.reset_resume = appleib_hid_reset_resume,
#endif
};

static struct appleib_device *appleib_alloc_device(struct acpi_device *acpi_dev)
{
	struct appleib_device *ib_dev;
	acpi_status sts;

	ib_dev = devm_kzalloc(&acpi_dev->dev, sizeof(*ib_dev), GFP_KERNEL);
	if (!ib_dev)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ib_dev->hid_drivers);
	INIT_LIST_HEAD(&ib_dev->hid_devices);
	mutex_init(&ib_dev->update_lock);
	init_srcu_struct(&ib_dev->lists_srcu);

	ib_dev->acpi_dev = acpi_dev;

	/* get iBridge acpi power control method for suspend/resume */
	sts = acpi_get_handle(acpi_dev->handle, "SOCW", &ib_dev->asoc_socw);
	if (ACPI_FAILURE(sts)) {
		dev_err(LOG_DEV(ib_dev),
			"Error getting handle for ASOC.SOCW method: %s\n",
			acpi_format_exception(sts));
		return ERR_PTR(-ENXIO);
	}

	/* ensure iBridge is powered on */
	sts = acpi_execute_simple_method(ib_dev->asoc_socw, NULL, 1);
	if (ACPI_FAILURE(sts))
		dev_warn(LOG_DEV(ib_dev), "SOCW(1) failed: %s\n",
			 acpi_format_exception(sts));

	return ib_dev;
}

static int appleib_probe(struct acpi_device *acpi)
{
	struct appleib_device *ib_dev;
	int i;
	int ret;

	ib_dev = appleib_alloc_device(acpi);
	if (IS_ERR_OR_NULL(ib_dev))
		return PTR_ERR(ib_dev);

	memcpy(ib_dev->subdevs, appleib_subdevs,
	       ARRAY_SIZE(ib_dev->subdevs) * sizeof(ib_dev->subdevs[0]));

	ib_dev->dev_data.ib_dev = ib_dev;
	ib_dev->dev_data.log_dev = LOG_DEV(ib_dev);

	for (i = 0; i < ARRAY_SIZE(ib_dev->subdevs); i++) {
		ib_dev->subdevs[i].platform_data = &ib_dev->dev_data;
		ib_dev->subdevs[i].pdata_size = sizeof(ib_dev->dev_data);
	}

	ret = devm_mfd_add_devices(&acpi->dev, PLATFORM_DEVID_NONE,
				   ib_dev->subdevs, ARRAY_SIZE(ib_dev->subdevs),
				   NULL, 0, NULL);
	if (ret) {
		dev_err(LOG_DEV(ib_dev), "Error adding MFD devices: %d\n", ret);
		return ret;
	}

	memcpy(ib_dev->ib_dev_ids, appleib_hid_ids,
	       ARRAY_SIZE(ib_dev->ib_dev_ids) * sizeof(ib_dev->ib_dev_ids[0]));
	memcpy(&ib_dev->ib_driver, &appleib_hid_driver,
	       sizeof(ib_dev->ib_driver));

	for (i = 0; i < ARRAY_SIZE(ib_dev->ib_dev_ids); i++)
		ib_dev->ib_dev_ids[i].driver_data = (kernel_ulong_t)ib_dev;

	ib_dev->ib_driver.id_table = ib_dev->ib_dev_ids;

	ret = hid_register_driver(&ib_dev->ib_driver);
	if (ret) {
		dev_err(LOG_DEV(ib_dev), "Error registering hid driver: %d\n",
			ret);
		return ret;
	}

	acpi->driver_data = ib_dev;

	return 0;
}

static int appleib_remove(struct acpi_device *acpi)
{
	struct appleib_device *ib_dev = acpi_driver_data(acpi);

	hid_unregister_driver(&ib_dev->ib_driver);

	return 0;
}

static int appleib_suspend(struct device *dev)
{
	struct appleib_device *ib_dev;
	int rc;

	ib_dev = acpi_driver_data(to_acpi_device(dev));

	rc = acpi_execute_simple_method(ib_dev->asoc_socw, NULL, 0);
	if (ACPI_FAILURE(rc))
		dev_warn(dev, "SOCW(0) failed: %s\n",
			 acpi_format_exception(rc));

	return 0;
}

static int appleib_resume(struct device *dev)
{
	struct appleib_device *ib_dev;
	int rc;

	ib_dev = acpi_driver_data(to_acpi_device(dev));

	rc = acpi_execute_simple_method(ib_dev->asoc_socw, NULL, 1);
	if (ACPI_FAILURE(rc))
		dev_warn(dev, "SOCW(1) failed: %s\n",
			 acpi_format_exception(rc));

	return 0;
}

static const struct dev_pm_ops appleib_pm = {
	.suspend = appleib_suspend,
	.resume = appleib_resume,
	.restore = appleib_resume,
};

static const struct acpi_device_id appleib_acpi_match[] = {
	{ "APP7777", 0 },
	{ },
};

MODULE_DEVICE_TABLE(acpi, appleib_acpi_match);

static struct acpi_driver appleib_driver = {
	.name		= "apple-ibridge",
	.class		= "topcase", /* ? */
	.owner		= THIS_MODULE,
	.ids		= appleib_acpi_match,
	.ops		= {
		.add		= appleib_probe,
		.remove		= appleib_remove,
	},
	.drv		= {
		.pm		= &appleib_pm,
	},
};

module_acpi_driver(appleib_driver)

MODULE_AUTHOR("Ronald Tschalär");
MODULE_DESCRIPTION("Apple iBridge driver");
MODULE_LICENSE("GPL v2");
