// SPDX-License-Identifier: GPL-2.0
/*
 * Apple Touchbar Driver
 *
 * Copyright (c) 2017-2018 Ronald Tschalär
 */

/**
 * MacBookPro models with a touchbar (13,[23] and 14,[23]) have an Apple
 * iBridge chip which exposes the touchbar and built-in webcam (iSight).
 * This shows up in the system as a USB device with 3 configurations:
 * 'Default iBridge Interfaces', 'Default iBridge Interfaces(OS X)', and
 * 'Default iBridge Interfaces(Recovery)'. While the second one is used by
 * MacOS to provide the fancy touchbar functionality with custom buttons
 * etc, this driver just uses the first.
 *
 * In the first (default after boot) configuration, 4 usb interfaces are
 * exposed: 2 related to the webcam, and 2 USB HID interfaces representing
 * the touchbar. The webcam interfaces are already handled by the uvcvideo
 * driver; furthermore, the handling of the input reports when "keys" on
 * the touchbar are pressed is already handled properly by the generic USB
 * HID core. This leaves the management of the touchbar modes (e.g.
 * switching between function and special keys when the FN key is pressed)
 * and the display (dimming and turning off), as well as the key-remapping
 * when the FN key is pressed, which are what this driver implements.
 *
 * The first USB HID interface supports two reports, an input report that
 * is used to report the key presses, and an output report which can be
 * used to set the touchbar "mode": touchbar off (in which case no touches
 * are reported at all), escape key only, escape + 12 function keys, and
 * escape + several special keys (including brightness, audio volume,
 * etc).  The second interface supports several, complex reports, most of
 * which are unknown at this time, but one of which has been determined to
 * allow for controlling of the touchbar's brightness: off (though touches
 * are still reported), dimmed, and full brightness. This driver makes
 * uses of these two reports.
 */

#define pr_fmt(fmt) "appletb: " fmt

#include <linux/module.h>
#include <linux/input.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/hid.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/acpi.h>
#include <linux/version.h>

#ifdef UPSTREAM
#include "usbhid/usbhid.h"
#else
#define	hid_to_usb_dev(hid_dev) \
	to_usb_device((hid_dev)->dev.parent->parent)
#endif

#define USB_ID_VENDOR_APPLE	0x05ac
#define USB_ID_PRODUCT_IBRIDGE	0x8600

#define APPLETB_BASIC_CONFIG	1

#define HID_UP_APPLE		0xff120000
#define HID_USAGE_MODE		(HID_UP_CUSTOM | 0x0004)
#define HID_USAGE_APPLE_APP	(HID_UP_APPLE  | 0x0001)
#define HID_USAGE_DISP		(HID_UP_APPLE  | 0x0021)

#define APPLETB_ACPI_ASOC_HID	"APP7777"

#define APPLETB_MAX_TB_KEYS	13	/* ESC, F1-F12 */

#define APPLETB_CMD_MODE_ESC	0
#define APPLETB_CMD_MODE_FN	1
#define APPLETB_CMD_MODE_SPCL	2
#define APPLETB_CMD_MODE_OFF	3
#define APPLETB_CMD_MODE_NONE	255

#define APPLETB_CMD_DISP_ON	1
#define APPLETB_CMD_DISP_DIM	2
#define APPLETB_CMD_DISP_OFF	4
#define APPLETB_CMD_DISP_NONE	255

#define APPLETB_FN_MODE_FKEYS	0
#define APPLETB_FN_MODE_NORM	1
#define APPLETB_FN_MODE_INV	2
#define APPLETB_FN_MODE_SPCL	3
#define APPLETB_FN_MODE_MAX	APPLETB_FN_MODE_SPCL

#define APPLETB_DEVID_TOUCHBAR	0
#define APPLETB_DEVID_KEYBOARD	1
#define APPLETB_DEVID_TOUCHPAD	2

#define APPLETB_MAX_DIM_TIME	30

static int appletb_tb_def_idle_timeout = 5 * 60;
module_param_named(idle_timeout, appletb_tb_def_idle_timeout, int, 0444);
MODULE_PARM_DESC(idle_timeout, "Default touchbar idle timeout (in seconds); 0 disables touchbar, -1 disables timeout");

static int appletb_tb_def_dim_timeout = -2;
module_param_named(dim_timeout, appletb_tb_def_dim_timeout, int, 0444);
MODULE_PARM_DESC(dim_timeout, "Default touchbar dim timeout (in seconds); 0 means always dimmmed, -1 disables dimming, -2 calculates timeout based on idle-timeout");

static int appletb_tb_def_fn_mode = APPLETB_FN_MODE_NORM;
module_param_named(fnmode, appletb_tb_def_fn_mode, int, 0444);
MODULE_PARM_DESC(fnmode, "Default FN key mode: 0 = f-keys only, 1 = fn key switches from special to f-keys, 2 = inverse of 1, 3 = special keys only");

static ssize_t idle_timeout_show(struct device *dev,
				 struct device_attribute *attr, char *buf);
static ssize_t idle_timeout_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size);
static DEVICE_ATTR_RW(idle_timeout);

static ssize_t dim_timeout_show(struct device *dev,
				struct device_attribute *attr, char *buf);
static ssize_t dim_timeout_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size);
static DEVICE_ATTR_RW(dim_timeout);

static ssize_t fnmode_show(struct device *dev, struct device_attribute *attr,
			   char *buf);
static ssize_t fnmode_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t size);
static DEVICE_ATTR_RW(fnmode);

static struct attribute *appletb_attrs[] = {
	&dev_attr_idle_timeout.attr,
	&dev_attr_dim_timeout.attr,
	&dev_attr_fnmode.attr,
	NULL,
};

static const struct attribute_group appletb_attr_group = {
	.attrs = appletb_attrs,
};

struct appletb_device {
	struct kref		kref;
	bool			active;

	acpi_handle		asoc_socw;

	struct appletb_report_info {
		struct hid_device	*hdev;
		struct usb_interface	*usb_iface;
		unsigned int		usb_epnum;
		unsigned int		report_id;
		unsigned int		report_type;
	}			mode_info, disp_info;

	struct input_handler	inp_handler;
	struct input_handle	kbd_handle;
	struct input_handle	tpd_handle;

	bool			last_tb_keys_pressed[APPLETB_MAX_TB_KEYS];
	bool			last_tb_keys_translated[APPLETB_MAX_TB_KEYS];
	bool			last_fn_pressed;

	ktime_t			last_event_time;

	unsigned char		cur_tb_mode;
	unsigned char		pnd_tb_mode;
	unsigned char		cur_tb_disp;
	unsigned char		pnd_tb_disp;
	bool			tb_autopm_off;
	struct delayed_work	tb_mode_work;
	/* protects the above mode flags */
	spinlock_t		tb_mode_lock;

	int			dim_timeout;
	int			idle_timeout;
	bool			dim_to_is_calc;
	int			fn_mode;
};

static struct appletb_device *appletb_dev;
DEFINE_MUTEX(appletb_dev_lock);		/* protect appletb_dev */

struct appletb_key_translation {
	u16 from;
	u16 to;
};

static const struct appletb_key_translation appletb_fn_codes[] = {
	{ KEY_F1,  KEY_BRIGHTNESSDOWN },
	{ KEY_F2,  KEY_BRIGHTNESSUP },
	{ KEY_F3,  KEY_SCALE },		/* not used */
	{ KEY_F4,  KEY_DASHBOARD },     /* not used */
	{ KEY_F5,  KEY_KBDILLUMDOWN },
	{ KEY_F6,  KEY_KBDILLUMUP },
	{ KEY_F7,  KEY_PREVIOUSSONG },
	{ KEY_F8,  KEY_PLAYPAUSE },
	{ KEY_F9,  KEY_NEXTSONG },
	{ KEY_F10, KEY_MUTE },
	{ KEY_F11, KEY_VOLUMEDOWN },
	{ KEY_F12, KEY_VOLUMEUP },
};

static int appletb_send_hid_report(struct appletb_report_info *rinfo,
				   __u8 requesttype, void *data, __u16 size)
{
	void *buffer;
	struct usb_device *dev = interface_to_usbdev(rinfo->usb_iface);
	u8 ifnum = rinfo->usb_iface->cur_altsetting->desc.bInterfaceNumber;
	int tries = 0;
	int rc;

	buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	memcpy(buffer, data, size);

	do {
		rc = usb_control_msg(dev,
				     usb_sndctrlpipe(dev, rinfo->usb_epnum),
				     HID_REQ_SET_REPORT, requesttype,
				     rinfo->report_type << 8 | rinfo->report_id,
				     ifnum, buffer, size, 2000);
		if (rc != -EPIPE)
			break;

		usleep_range(1000 << tries, 3000 << tries);
	} while (++tries < 5);

	kfree(buffer);

	return (rc > 0) ? 0 : rc;
}

static int appletb_set_tb_mode(struct appletb_device *tb_dev,
			       unsigned char mode)
{
	int rc;

	if (!tb_dev->mode_info.usb_iface)
		return -1;

	rc = appletb_send_hid_report(&tb_dev->mode_info,
				     USB_DIR_OUT | USB_TYPE_VENDOR |
							USB_RECIP_DEVICE,
				     &mode, 1);
	if (rc < 0)
		pr_err("Failed to set touchbar mode to %u (%d)\n", mode, rc);

	return rc;
}

static int appletb_set_tb_disp(struct appletb_device *tb_dev,
			       unsigned char disp)
{
	unsigned char report[] = { 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	int rc;

	if (!tb_dev->disp_info.usb_iface)
		return -1;

	/*
	 * Keep the USB interface powered on while the touchbar display is on
	 * for better responsiveness.
	 */
	if (disp != APPLETB_CMD_DISP_OFF &&
	    tb_dev->cur_tb_disp == APPLETB_CMD_DISP_OFF) {
		rc = usb_autopm_get_interface(tb_dev->disp_info.usb_iface);
		if (rc == 0)
			tb_dev->tb_autopm_off = true;
		else
			pr_err("Failed to disable auto-pm on touchbar device (%d)\n",
			       rc);
	}

	report[0] = tb_dev->disp_info.report_id;
	report[2] = disp;

	rc = appletb_send_hid_report(&tb_dev->disp_info,
				     USB_DIR_OUT | USB_TYPE_CLASS |
						USB_RECIP_INTERFACE,
				     report, sizeof(report));
	if (rc < 0)
		pr_err("Failed to set touchbar display to %u (%d)\n", disp, rc);

	if (disp == APPLETB_CMD_DISP_OFF &&
	    tb_dev->cur_tb_disp != APPLETB_CMD_DISP_OFF) {
		if (tb_dev->tb_autopm_off) {
			usb_autopm_put_interface(tb_dev->disp_info.usb_iface);
			tb_dev->tb_autopm_off = false;
		}
	}

	return rc;
}

static bool appletb_any_tb_key_pressed(struct appletb_device *tb_dev)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(tb_dev->last_tb_keys_pressed); idx++) {
		if (tb_dev->last_tb_keys_pressed[idx])
			return true;
	}

	return false;
}

static void appletb_set_tb_mode_worker(struct work_struct *work)
{
	struct appletb_device *tb_dev =
		container_of(work, struct appletb_device, tb_mode_work.work);
	s64 time_left, min_timeout, time_to_off;
	unsigned char pending_mode;
	unsigned char pending_disp;
	unsigned char current_disp;
	bool any_tb_key_pressed, need_reschedule;
	int rc1 = 1, rc2 = 1;
	unsigned long flags;

	spin_lock_irqsave(&tb_dev->tb_mode_lock, flags);

	/* handle explicit mode-change request */
	pending_mode = tb_dev->pnd_tb_mode;
	pending_disp = tb_dev->pnd_tb_disp;

	spin_unlock_irqrestore(&tb_dev->tb_mode_lock, flags);

	if (pending_mode != APPLETB_CMD_MODE_NONE)
		rc1 = appletb_set_tb_mode(tb_dev, pending_mode);
	if (pending_disp != APPLETB_CMD_DISP_NONE)
		rc2 = appletb_set_tb_disp(tb_dev, pending_disp);

	spin_lock_irqsave(&tb_dev->tb_mode_lock, flags);

	need_reschedule = false;

	if (rc1 == 0) {
		tb_dev->cur_tb_mode = pending_mode;

		if (tb_dev->pnd_tb_mode == pending_mode)
			tb_dev->pnd_tb_mode = APPLETB_CMD_MODE_NONE;
		else
			need_reschedule = true;
	}

	if (rc2 == 0) {
		tb_dev->cur_tb_disp = pending_disp;

		if (tb_dev->pnd_tb_disp == pending_disp)
			tb_dev->pnd_tb_disp = APPLETB_CMD_DISP_NONE;
		else
			need_reschedule = true;
	}
	current_disp = tb_dev->cur_tb_disp;

	/* calculate time left to next timeout */
	if (tb_dev->idle_timeout <= 0 && tb_dev->dim_timeout <= 0)
		min_timeout = -1;
	else if (tb_dev->dim_timeout <= 0)
		min_timeout = tb_dev->idle_timeout;
	else if (tb_dev->idle_timeout <= 0)
		min_timeout = tb_dev->dim_timeout;
	else
		min_timeout = min(tb_dev->dim_timeout, tb_dev->idle_timeout);

	if (min_timeout > 0) {
		s64 idle_time =
			(ktime_ms_delta(ktime_get(), tb_dev->last_event_time) +
			 500) / 1000;

		time_left = max(min_timeout - idle_time, 0LL);
		if (tb_dev->idle_timeout <= 0)
			time_to_off = -1;
		else if (idle_time >= tb_dev->idle_timeout)
			time_to_off = 0;
		else
			time_to_off = tb_dev->idle_timeout - idle_time;
	}

	any_tb_key_pressed = appletb_any_tb_key_pressed(tb_dev);

	spin_unlock_irqrestore(&tb_dev->tb_mode_lock, flags);

	/* a new command arrived while we were busy - handle it */
	if (need_reschedule) {
		schedule_delayed_work(&tb_dev->tb_mode_work, 0);
		return;
	}

	/* if no idle/dim timeout, we're done */
	if (min_timeout <= 0)
		return;

	/* manage idle/dim timeout */
	if (time_left > 0) {
		/* we fired too soon or had a mode-change- re-schedule */
		schedule_delayed_work(&tb_dev->tb_mode_work,
				      msecs_to_jiffies(time_left * 1000));
	} else if (any_tb_key_pressed) {
		/* keys are still pressed - re-schedule */
		schedule_delayed_work(&tb_dev->tb_mode_work,
				      msecs_to_jiffies(min_timeout * 1000));
	} else {
		/* dim or idle timeout reached */
		int next_disp = (time_to_off == 0) ? APPLETB_CMD_DISP_OFF :
						     APPLETB_CMD_DISP_DIM;
		if (next_disp != current_disp &&
		    appletb_set_tb_disp(tb_dev, next_disp) == 0) {
			spin_lock_irqsave(&tb_dev->tb_mode_lock, flags);
			tb_dev->cur_tb_disp = next_disp;
			spin_unlock_irqrestore(&tb_dev->tb_mode_lock, flags);
		}

		if (time_to_off > 0)
			schedule_delayed_work(&tb_dev->tb_mode_work,
					msecs_to_jiffies(time_to_off * 1000));
	}
}

static u16 appletb_fn_to_special(u16 code)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(appletb_fn_codes); idx++) {
		if (appletb_fn_codes[idx].from == code)
			return appletb_fn_codes[idx].to;
	}

	return 0;
}

static unsigned char appletb_get_cur_tb_mode(struct appletb_device *tb_dev)
{
	return tb_dev->pnd_tb_mode != APPLETB_CMD_MODE_NONE ?
				tb_dev->pnd_tb_mode : tb_dev->cur_tb_mode;
}

static unsigned char appletb_get_cur_tb_disp(struct appletb_device *tb_dev)
{
	return tb_dev->pnd_tb_disp != APPLETB_CMD_DISP_NONE ?
				tb_dev->pnd_tb_disp : tb_dev->cur_tb_disp;
}

static unsigned char appletb_get_fn_tb_mode(struct appletb_device *tb_dev)
{
	switch (tb_dev->fn_mode) {
	case APPLETB_FN_MODE_FKEYS:
		return APPLETB_CMD_MODE_FN;

	case APPLETB_FN_MODE_SPCL:
		return APPLETB_CMD_MODE_SPCL;

	case APPLETB_FN_MODE_INV:
		return (tb_dev->last_fn_pressed) ? APPLETB_CMD_MODE_SPCL :
						   APPLETB_CMD_MODE_FN;

	case APPLETB_FN_MODE_NORM:
	default:
		return (tb_dev->last_fn_pressed) ? APPLETB_CMD_MODE_FN :
						   APPLETB_CMD_MODE_SPCL;
	}
}

/*
 * Switch touchbar mode and display when mode or display not the desired ones.
 */
static void appletb_update_touchbar_no_lock(struct appletb_device *tb_dev,
					    bool force)
{
	unsigned char want_mode;
	unsigned char want_disp;
	bool need_update = false;

	/*
	 * Calculate the new modes:
	 *   idle_timeout:
	 *     -1  always on
	 *      0  always off
	 *     >0  turn off after idle_timeout seconds
	 *   dim_timeout (only valid if idle_timeout != 0):
	 *     -1  never dimmed
	 *      0  always dimmed
	 *     >0  dim off after dim_timeout seconds
	 */
	if (tb_dev->idle_timeout == 0) {
		want_mode = APPLETB_CMD_MODE_OFF;
		want_disp = APPLETB_CMD_DISP_OFF;
	} else {
		want_mode = appletb_get_fn_tb_mode(tb_dev);
		want_disp = tb_dev->dim_timeout == 0 ? APPLETB_CMD_DISP_DIM :
						       APPLETB_CMD_DISP_ON;
	}

	/*
	 * See if we need to update the touchbar, taking into account that we
	 * generally don't want to switch modes while a touchbar key is pressed.
	 */
	if (appletb_get_cur_tb_mode(tb_dev) != want_mode &&
	    !appletb_any_tb_key_pressed(tb_dev)) {
		tb_dev->pnd_tb_mode = want_mode;
		need_update = true;
	}

	if (appletb_get_cur_tb_disp(tb_dev) != want_disp &&
	    (!appletb_any_tb_key_pressed(tb_dev) ||
	     (appletb_any_tb_key_pressed(tb_dev) &&
	      want_disp != APPLETB_CMD_DISP_OFF))) {
		tb_dev->pnd_tb_disp = want_disp;
		need_update = true;
	}

	if (force)
		need_update = true;

	/* schedule the update if desired */
	pr_debug_ratelimited("update: need_update=%d, want_mode=%d, cur-mode=%d, want_disp=%d, cur-disp=%d\n",
			     need_update, want_mode, tb_dev->cur_tb_mode,
			     want_disp, tb_dev->cur_tb_disp);
	if (need_update) {
		cancel_delayed_work(&tb_dev->tb_mode_work);
		schedule_delayed_work(&tb_dev->tb_mode_work, 0);
	}
}

static void appletb_update_touchbar(struct appletb_device *tb_dev, bool force)
{
	unsigned long flags;

	spin_lock_irqsave(&tb_dev->tb_mode_lock, flags);

	if (tb_dev->active)
		appletb_update_touchbar_no_lock(tb_dev, force);

	spin_unlock_irqrestore(&tb_dev->tb_mode_lock, flags);
}

static void appletb_set_idle_timeout(struct appletb_device *tb_dev, int new)
{
	tb_dev->idle_timeout = new;
	if (tb_dev->dim_to_is_calc)
		tb_dev->dim_timeout = new - min(APPLETB_MAX_DIM_TIME, new / 3);
}

static ssize_t idle_timeout_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct appletb_device *tb_dev = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", tb_dev->idle_timeout);
}

static ssize_t idle_timeout_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	struct appletb_device *tb_dev = dev_get_drvdata(dev);
	long new;
	int rc;

	rc = kstrtol(buf, 0, &new);
	if (rc || new > INT_MAX || new < -1)
		return -EINVAL;

	appletb_set_idle_timeout(tb_dev, new);
	appletb_update_touchbar(tb_dev, true);

	return size;
}

static void appletb_set_dim_timeout(struct appletb_device *tb_dev, int new)
{
	if (new == -2) {
		tb_dev->dim_to_is_calc = true;
		appletb_set_idle_timeout(tb_dev, tb_dev->idle_timeout);
	} else {
		tb_dev->dim_to_is_calc = false;
		tb_dev->dim_timeout = new;
	}
}

static ssize_t dim_timeout_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct appletb_device *tb_dev = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n",
			tb_dev->dim_to_is_calc ? -2 : tb_dev->dim_timeout);
}

static ssize_t dim_timeout_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct appletb_device *tb_dev = dev_get_drvdata(dev);
	long new;
	int rc;

	rc = kstrtol(buf, 0, &new);
	if (rc || new > INT_MAX || new < -2)
		return -EINVAL;

	appletb_set_dim_timeout(tb_dev, new);
	appletb_update_touchbar(tb_dev, true);

	return size;
}

static ssize_t fnmode_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct appletb_device *tb_dev = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d\n", tb_dev->fn_mode);
}

static ssize_t fnmode_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t size)
{
	struct appletb_device *tb_dev = dev_get_drvdata(dev);
	long new;
	int rc;

	rc = kstrtol(buf, 0, &new);
	if (rc || new > APPLETB_FN_MODE_MAX || new < 0)
		return -EINVAL;

	tb_dev->fn_mode = new;
	appletb_update_touchbar(tb_dev, false);

	return size;
}

static int appletb_tb_key_to_slot(unsigned int code)
{
	switch (code) {
	case KEY_ESC:
		return 0;
	case KEY_F1:
	case KEY_F2:
	case KEY_F3:
	case KEY_F4:
	case KEY_F5:
	case KEY_F6:
	case KEY_F7:
	case KEY_F8:
	case KEY_F9:
	case KEY_F10:
		return code - KEY_F1 + 1;
	case KEY_F11:
	case KEY_F12:
		return code - KEY_F11 + 11;
	default:
		return -1;
	}
}

static int appletb_tb_event(struct hid_device *hdev, struct hid_field *field,
			    struct hid_usage *usage, __s32 value)
{
	struct appletb_device *tb_dev = hid_get_drvdata(hdev);
	unsigned long flags;
	unsigned int new_code = 0;
	bool send_dummy = false;
	bool send_trnsl = false;
	int slot;
	int rc = 0;

	if ((usage->hid & HID_USAGE_PAGE) != HID_UP_KEYBOARD)
		return 0;

	/*
	 * Skip non-touchbar keys.
	 *
	 * Either the touchbar itself or usbhid generate a slew of key-down
	 * events for all the meta keys. None of which we're at all interested
	 * in.
	 */
	slot = appletb_tb_key_to_slot(usage->code);
	if (slot < 0)
		return 0;

	if (usage->type == EV_KEY)
		new_code = appletb_fn_to_special(usage->code);

	spin_lock_irqsave(&tb_dev->tb_mode_lock, flags);

	if (!tb_dev->active) {
		spin_unlock_irqrestore(&tb_dev->tb_mode_lock, flags);
		return 0;
	}

	/* remember which (untranslated) touchbar keys are pressed */
	if (usage->type == EV_KEY && value != 2)
		tb_dev->last_tb_keys_pressed[slot] = value;

	/* remember last time keyboard or touchpad was touched */
	tb_dev->last_event_time = ktime_get();

	/* only switch touchbar mode when no touchbar keys are pressed */
	appletb_update_touchbar_no_lock(tb_dev, false);

	/*
	 * We want to suppress touchbar keys while the touchbar is off, but we
	 * do want to wake up the screen if it's asleep, so generate a dummy
	 * event.
	 */
	if (tb_dev->cur_tb_mode == APPLETB_CMD_MODE_OFF ||
	    tb_dev->cur_tb_disp == APPLETB_CMD_DISP_OFF) {
		send_dummy = true;
		rc = 1;
	/* translate special keys */
	} else if (usage->type == EV_KEY && new_code &&
		   ((value > 0 &&
		     appletb_get_cur_tb_mode(tb_dev) == APPLETB_CMD_MODE_SPCL)
		    ||
		    (value == 0 && tb_dev->last_tb_keys_translated[slot]))) {
		tb_dev->last_tb_keys_translated[slot] = true;
		send_trnsl = true;
		rc = 1;
	/* everything else handled normally */
	} else {
		tb_dev->last_tb_keys_translated[slot] = false;
	}

	spin_unlock_irqrestore(&tb_dev->tb_mode_lock, flags);

	/*
	 * Need to send these input events outside of the lock, as otherwise
	 * we can run into the following deadlock:
	 *            Task 1                         Task 2
	 *     appletb_tb_event()             input_event()
	 *       acquire tb_mode_lock           acquire dev->event_lock
	 *       input_event()                  appletb_inp_event()
	 *         acquire dev->event_lock        acquire tb_mode_lock
	 */
	if (send_dummy) {
		input_event(field->hidinput->input, EV_KEY, KEY_UNKNOWN, 1);
		input_event(field->hidinput->input, EV_KEY, KEY_UNKNOWN, 0);
	} else if (send_trnsl) {
		input_event(field->hidinput->input, usage->type, new_code,
			    value);
	}

	return rc;
}

static void appletb_inp_event(struct input_handle *handle, unsigned int type,
			      unsigned int code, int value)
{
	struct appletb_device *tb_dev = handle->private;
	unsigned long flags;

	spin_lock_irqsave(&tb_dev->tb_mode_lock, flags);

	if (!tb_dev->active) {
		spin_unlock_irqrestore(&tb_dev->tb_mode_lock, flags);
		return;
	}

	/* remember last state of FN key */
	if (type == EV_KEY && code == KEY_FN && value != 2)
		tb_dev->last_fn_pressed = value;

	/* remember last time keyboard or touchpad was touched */
	tb_dev->last_event_time = ktime_get();

	/* only switch touchbar mode when no touchbar keys are pressed */
	appletb_update_touchbar_no_lock(tb_dev, false);

	spin_unlock_irqrestore(&tb_dev->tb_mode_lock, flags);
}

/* Find and save the usb-device associated with the touchbar input device */
static struct usb_interface *appletb_get_usb_iface(struct hid_device *hdev)
{
	struct device *dev = &hdev->dev;

	/* find the usb-interface device */
	if (!dev->bus || strcmp(dev->bus->name, "hid") != 0)
		return NULL;

	dev = dev->parent;
	if (!dev || !dev->bus || strcmp(dev->bus->name, "usb") != 0)
		return NULL;

	return to_usb_interface(dev);
}

static int appletb_inp_connect(struct input_handler *handler,
			       struct input_dev *dev,
			       const struct input_device_id *id)
{
	struct appletb_device *tb_dev = handler->private;
	struct input_handle *handle;
	int rc;

	if (id->driver_info == APPLETB_DEVID_KEYBOARD) {
		handle = &tb_dev->kbd_handle;
		handle->name = "tbkbd";
	} else if (id->driver_info == APPLETB_DEVID_TOUCHPAD) {
		handle = &tb_dev->tpd_handle;
		handle->name = "tbtpad";
	} else {
		pr_err("Unknown device id (%lu)\n", id->driver_info);
		return -ENOENT;
	}

	if (handle->dev) {
		pr_err("Duplicate connect to %s input device\n", handle->name);
		return -EEXIST;
	}

	handle->open = 0;
	handle->dev = input_get_device(dev);
	handle->handler = handler;
	handle->private = tb_dev;

	rc = input_register_handle(handle);
	if (rc)
		goto err_free_dev;

	rc = input_open_device(handle);
	if (rc)
		goto err_unregister_handle;

	pr_info("Connected to %s input device\n",
		handle == &tb_dev->kbd_handle ? "keyboard" : "touchpad");

	return 0;

 err_unregister_handle:
	input_unregister_handle(handle);
 err_free_dev:
	input_put_device(handle->dev);
	handle->dev = NULL;
	return rc;
}

static void appletb_inp_disconnect(struct input_handle *handle)
{
	struct appletb_device *tb_dev = handle->private;

	input_close_device(handle);
	input_unregister_handle(handle);

	input_put_device(handle->dev);
	handle->dev = NULL;

	pr_info("Disconnected from %s input device\n",
		handle == &tb_dev->kbd_handle ? "keyboard" : "touchpad");
}

static int appletb_input_configured(struct hid_device *hdev,
				    struct hid_input *hidinput)
{
	int idx;
	struct input_dev *input = hidinput->input;
	#define CLEAR_ARRAY(array) \
		memset(array, 0, sizeof(array))

	/*
	 * Clear various input capabilities that are blindly set by the hid
	 * driver (usbkbd.c)
	 */
	CLEAR_ARRAY(input->evbit);
	CLEAR_ARRAY(input->keybit);
	CLEAR_ARRAY(input->ledbit);

	/* set our actual capabilities */
	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_REP, input->evbit);
	__set_bit(EV_MSC, input->evbit);  /* hid-input generates MSC_SCAN */

	for (idx = 0; idx < ARRAY_SIZE(appletb_fn_codes); idx++) {
		input_set_capability(input, EV_KEY, appletb_fn_codes[idx].from);
		input_set_capability(input, EV_KEY, appletb_fn_codes[idx].to);
	}
	input_set_capability(input, EV_KEY, KEY_ESC);
	input_set_capability(input, EV_KEY, KEY_UNKNOWN);

	return 0;
}

static struct hid_field *appletb_find_hid_field(struct hid_device *hdev,
						unsigned int application,
						unsigned int field_usage)
{
	static const int report_types[] = { HID_INPUT_REPORT, HID_OUTPUT_REPORT,
					    HID_FEATURE_REPORT };
	struct hid_report *report;
	int t, f, u;

	for (t = 0; t < ARRAY_SIZE(report_types); t++) {
		struct list_head *report_list =
			    &hdev->report_enum[report_types[t]].report_list;
		list_for_each_entry(report, report_list, list) {
			if (report->application != application)
				continue;

			for (f = 0; f < report->maxfield; f++) {
				struct hid_field *field = report->field[f];

				if (field->logical == field_usage)
					return field;

				for (u = 0; u < field->maxusage; u++) {
					if (field->usage[u].hid == field_usage)
						return field;
				}
			}
		}
	}

	return NULL;
}

static int appletb_fill_report_info(struct appletb_device *tb_dev,
				    struct hid_device *hdev)
{
	struct appletb_report_info *report_info = NULL;
	struct usb_interface *usb_iface;
	struct hid_field *field;

	field = appletb_find_hid_field(hdev, HID_GD_KEYBOARD, HID_USAGE_MODE);
	if (field) {
		report_info = &tb_dev->mode_info;
	} else {
		field = appletb_find_hid_field(hdev, HID_USAGE_APPLE_APP,
					       HID_USAGE_DISP);
		if (field)
			report_info = &tb_dev->disp_info;
	}

	if (!report_info)
		return 0;

	usb_iface = appletb_get_usb_iface(hdev);
	if (!usb_iface) {
		hid_err(hdev, "Failed to find usb interface for hid device\n");
		return -ENODEV;
	}

	report_info->hdev = hdev;

	report_info->usb_iface = usb_get_intf(usb_iface);
	report_info->usb_epnum = 0;

	report_info->report_id = field->report->id;
	switch (field->report->type) {
	case HID_INPUT_REPORT:
		report_info->report_type = 0x01; break;
	case HID_OUTPUT_REPORT:
		report_info->report_type = 0x02; break;
	case HID_FEATURE_REPORT:
		report_info->report_type = 0x03; break;
	}

	return 1;
}

static struct appletb_report_info *appletb_get_report_info(
						struct appletb_device *tb_dev,
						struct hid_device *hdev)
{
	if (hdev == tb_dev->mode_info.hdev)
		return &tb_dev->mode_info;
	if (hdev == tb_dev->disp_info.hdev)
		return &tb_dev->disp_info;
	return NULL;
}

static void appletb_mark_active(struct appletb_device *tb_dev, bool active)
{
	unsigned long flags;

	spin_lock_irqsave(&tb_dev->tb_mode_lock, flags);
	tb_dev->active = active;
	spin_unlock_irqrestore(&tb_dev->tb_mode_lock, flags);
}

static acpi_status appletb_get_acpi_handle_cb(acpi_handle object,
					      u32 nesting_level, void *context,
					      void **return_value)
{
	*return_value = object;
	return AE_CTRL_TERMINATE;
}

struct appletb_device *appletb_alloc_device(void)
{
	struct appletb_device *tb_dev;
	acpi_handle asoc_handle;
	acpi_status sts;
	int rc;

	/* allocate */
	tb_dev = kzalloc(sizeof(*tb_dev), GFP_KERNEL);
	if (!tb_dev)
		return ERR_PTR(-ENOMEM);

	/* initialize structures */
	kref_init(&tb_dev->kref);
	spin_lock_init(&tb_dev->tb_mode_lock);
	INIT_DELAYED_WORK(&tb_dev->tb_mode_work, appletb_set_tb_mode_worker);

	/* get iBridge acpi power control method */
	sts = acpi_get_devices(APPLETB_ACPI_ASOC_HID,
			       appletb_get_acpi_handle_cb, NULL, &asoc_handle);
	if (ACPI_FAILURE(sts)) {
		pr_err("Error getting handle for ACPI ASOC device: %s\n",
		       acpi_format_exception(sts));
		rc = -ENXIO;
		goto free_mem;
	}

	sts = acpi_get_handle(asoc_handle, "SOCW", &tb_dev->asoc_socw);
	if (ACPI_FAILURE(sts)) {
		pr_err("Error getting handle for ASOC.SOCW method: %s\n",
		       acpi_format_exception(sts));
		rc = -ENXIO;
		goto free_mem;
	}

	/* ensure iBridge is powered on */
	sts = acpi_execute_simple_method(tb_dev->asoc_socw, NULL, 1);
	if (ACPI_FAILURE(sts))
		pr_warn("SOCW(1) failed: %s\n", acpi_format_exception(sts));

	return tb_dev;

free_mem:
	kfree(tb_dev);
	return ERR_PTR(rc);
}

static void appletb_free_device(struct kref *kref)
{
	struct appletb_device *tb_dev =
		container_of(kref, struct appletb_device, kref);

	kfree(tb_dev);
	appletb_dev = NULL;
}

static const struct input_device_id appletb_input_devices[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_BUS |
			INPUT_DEVICE_ID_MATCH_KEYBIT,
		.bustype = BUS_SPI,
		.keybit = { [BIT_WORD(KEY_FN)] = BIT_MASK(KEY_FN) },
		.driver_info = APPLETB_DEVID_KEYBOARD,
	},			/* Builtin keyboard device */
	{
		.flags = INPUT_DEVICE_ID_MATCH_BUS |
			INPUT_DEVICE_ID_MATCH_KEYBIT,
		.bustype = BUS_SPI,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.driver_info = APPLETB_DEVID_TOUCHPAD,
	},			/* Builtin touchpad device */
	{ },			/* Terminating zero entry */
};

static int appletb_probe(struct hid_device *hdev,
			 const struct hid_device_id *id)
{
	struct appletb_device *tb_dev;
	struct appletb_report_info *report_info;
	struct usb_device *udev;
	int rc;

	/* check usb config first */
	udev = hid_to_usb_dev(hdev);

	if (udev->actconfig->desc.bConfigurationValue != APPLETB_BASIC_CONFIG) {
		rc = usb_driver_set_configuration(udev, APPLETB_BASIC_CONFIG);
		return rc ? rc : -ENODEV;
	}

	/* Allocate the driver data */
	mutex_lock(&appletb_dev_lock);

	if (!appletb_dev) {
		appletb_dev = appletb_alloc_device();
		if (IS_ERR_OR_NULL(appletb_dev)) {
			rc = PTR_ERR(appletb_dev);
			appletb_dev = NULL;
			goto unlock;
		}
	} else {
		kref_get(&appletb_dev->kref);
	}

	tb_dev = appletb_dev;

	hid_set_drvdata(hdev, tb_dev);

	/* initialize the report info */
	rc = hid_parse(hdev);
	if (rc) {
		hid_err(hdev, "hid parse failed (%d)\n", rc);
		goto free_mem;
	}

	rc = appletb_fill_report_info(tb_dev, hdev);
	if (rc < 0)
		goto free_mem;

	/* start the hid */
	rc = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (rc) {
		hid_err(hdev, "hw start failed (%d)\n", rc);
		goto free_iface;
	}

	/* do setup if we have both interfaces */
	if (tb_dev->mode_info.hdev && tb_dev->disp_info.hdev) {
		/* mark active */
		appletb_mark_active(tb_dev, true);

		/* initialize the touchbar */
		if (appletb_tb_def_fn_mode >= 0 &&
		    appletb_tb_def_fn_mode <= APPLETB_FN_MODE_MAX)
			tb_dev->fn_mode = appletb_tb_def_fn_mode;
		else
			tb_dev->fn_mode = APPLETB_FN_MODE_NORM;
		appletb_set_idle_timeout(tb_dev, appletb_tb_def_idle_timeout);
		appletb_set_dim_timeout(tb_dev, appletb_tb_def_dim_timeout);
		tb_dev->last_event_time = ktime_get();

		tb_dev->cur_tb_mode = APPLETB_CMD_MODE_OFF;
		tb_dev->cur_tb_disp = APPLETB_CMD_DISP_OFF;

		appletb_update_touchbar(tb_dev, false);

		/* set up the input handler */
		tb_dev->inp_handler.event = appletb_inp_event;
		tb_dev->inp_handler.connect = appletb_inp_connect;
		tb_dev->inp_handler.disconnect = appletb_inp_disconnect;
		tb_dev->inp_handler.name = "appletb";
		tb_dev->inp_handler.id_table = appletb_input_devices;
		tb_dev->inp_handler.private = tb_dev;

		rc = input_register_handler(&tb_dev->inp_handler);
		if (rc) {
			pr_err("Unabled to register keyboard handler (%d)\n",
			       rc);
			goto cancel_work;
		}

		/* initialize sysfs attributes */
		rc = sysfs_create_group(&tb_dev->mode_info.hdev->dev.kobj,
					&appletb_attr_group);
		if (rc) {
			pr_err("Failed to create sysfs attributes (%d)\n", rc);
			goto unreg_handler;
		}
	}

	/* done */
	mutex_unlock(&appletb_dev_lock);

	hid_info(hdev, "device probe done.\n");

	return 0;

unreg_handler:
	input_unregister_handler(&tb_dev->inp_handler);
cancel_work:
	cancel_delayed_work_sync(&tb_dev->tb_mode_work);
	appletb_mark_active(tb_dev, false);
	hid_hw_stop(hdev);
free_iface:
	report_info = appletb_get_report_info(tb_dev, hdev);
	if (report_info) {
		usb_put_intf(report_info->usb_iface);
		report_info->usb_iface = NULL;
		report_info->hdev = NULL;
	}
free_mem:
	kref_put(&tb_dev->kref, appletb_free_device);
unlock:
	mutex_unlock(&appletb_dev_lock);

	return rc;
}

static void appletb_remove(struct hid_device *hdev)
{
	struct appletb_device *tb_dev = hid_get_drvdata(hdev);
	struct appletb_report_info *report_info;

	mutex_lock(&appletb_dev_lock);

	hid_hw_stop(hdev);

	if ((hdev == tb_dev->mode_info.hdev && tb_dev->disp_info.hdev) ||
	    (hdev == tb_dev->disp_info.hdev && tb_dev->mode_info.hdev)) {
		sysfs_remove_group(&tb_dev->mode_info.hdev->dev.kobj,
				   &appletb_attr_group);

		input_unregister_handler(&tb_dev->inp_handler);

		cancel_delayed_work_sync(&tb_dev->tb_mode_work);
		appletb_set_tb_mode(tb_dev, APPLETB_CMD_MODE_OFF);
		appletb_set_tb_disp(tb_dev, APPLETB_CMD_DISP_ON);

		if (tb_dev->tb_autopm_off)
			usb_autopm_put_interface(tb_dev->disp_info.usb_iface);

		appletb_mark_active(tb_dev, false);
	}

	report_info = appletb_get_report_info(tb_dev, hdev);
	if (report_info) {
		usb_put_intf(report_info->usb_iface);
		report_info->usb_iface = NULL;
		report_info->hdev = NULL;
	}

	kref_put(&tb_dev->kref, appletb_free_device);

	mutex_unlock(&appletb_dev_lock);

	hid_info(hdev, "device remove done.\n");
}

#ifdef CONFIG_PM
static int appletb_suspend(struct hid_device *hdev, pm_message_t message)
{
	struct appletb_device *tb_dev = hid_get_drvdata(hdev);
	int rc;

	if (hdev != tb_dev->mode_info.hdev)
		return 0;

	if (message.event != PM_EVENT_SUSPEND &&
	    message.event != PM_EVENT_FREEZE)
		return 0;

	/* put iBridge to sleep */
	rc = acpi_execute_simple_method(tb_dev->asoc_socw, NULL, 0);
	if (ACPI_FAILURE(rc))
		pr_warn("SOCW(0) failed: %s\n", acpi_format_exception(rc));

	hid_info(hdev, "device suspend done.\n");

	return 0;
}

static int appletb_reset_resume(struct hid_device *hdev)
{
	struct appletb_device *tb_dev = hid_get_drvdata(hdev);
	int rc;

	if (hdev != tb_dev->mode_info.hdev)
		return 0;

	/* wake up iBridge */
	rc = acpi_execute_simple_method(tb_dev->asoc_socw, NULL, 1);
	if (ACPI_FAILURE(rc))
		pr_warn("SOCW(1) failed: %s\n", acpi_format_exception(rc));

	/* restore touchbar state */
	appletb_set_tb_mode(tb_dev, tb_dev->cur_tb_mode);
	appletb_set_tb_disp(tb_dev, tb_dev->cur_tb_disp);
	tb_dev->last_event_time = ktime_get();

	hid_info(hdev, "device resume done.\n");

	return 0;
}
#endif

static const struct hid_device_id appletb_touchbar_devices[] = {
	{
		HID_USB_DEVICE(USB_ID_VENDOR_APPLE, USB_ID_PRODUCT_IBRIDGE),
		.driver_data = APPLETB_DEVID_TOUCHBAR,
	},			/* Touchbar device */
	{ },
};
MODULE_DEVICE_TABLE(hid, appletb_touchbar_devices);

static struct hid_driver appletb_driver = {
	.name = "apple-touchbar",
	.id_table = appletb_touchbar_devices,
	.probe = appletb_probe,
	.remove = appletb_remove,
	.event = appletb_tb_event,
	.input_configured = appletb_input_configured,
#ifdef CONFIG_PM
	.suspend = appletb_suspend,
	.reset_resume = appletb_reset_resume,
#endif
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
module_hid_driver(appletb_driver);

#else /* KERNEL_VERSION >= 4.16 */
/*
 * Hack to work around the fact that it's not possible to dynamically be added
 * to hid-core's hid_ignore_list. This means the hid-generic hid driver will
 * always get attached to the touchbar device. So we listen on the usb bus for
 * the device to appear, release the hid-generic driver (if attached), and
 * trigger our driver instead.
 */

static struct {
	struct work_struct work;
	struct hid_device  *hdev;
} appletb_usb_hack_reg_data;

static void appletb_usb_hack_release_hid_dev(struct hid_device *hdev)
{
	struct device *dev = get_device(&hdev->dev);

	if (dev->parent)
		device_lock(dev->parent);
	device_release_driver(dev);
	if (dev->parent)
		device_unlock(dev->parent);

	put_device(dev);
}

static void appletb_usb_hack_reg_hid_driver(struct work_struct *work)
{
	struct hid_device *hid;
	int rc;

	/* check if a hid driver is attached */
	hid = appletb_usb_hack_reg_data.hdev;

	if (!hid || !hid->driver) {
		pr_debug("No hid driver attached to touchbar");

	/* check if we got attached */
	} else if (strcmp(hid->driver->name, appletb_driver.name) == 0) {
		pr_debug("Already attached");

	/* else normally expect hid-generic to be the one attached */
	} else if (strcmp(hid->driver->name, "hid-generic") != 0) {
		pr_warn("Unexpected hid driver '%s' attached to touchbar",
			hid->driver->name);

	} else {
		/*
		 * Detach current driver, and re-register ourselves in order to
		 * trigger attachment.
		 */
		pr_info("releasing current hid driver '%s'\n",
			hid->driver->name);
		appletb_usb_hack_release_hid_dev(hid);

		hid_unregister_driver(&appletb_driver);
		rc = hid_register_driver(&appletb_driver);
		if (rc)
			pr_err("Error (re)registering touchbar hid driver (%d)",
			       rc);
	}

	put_device(&hid->dev);
}

/*
 * hid_bus_type is not exported, so this is an ugly hack to get it anyway.
 */
static struct bus_type *appletb_usb_hack_get_usb_bus(void)
{
	struct hid_device *hid;
	struct bus_type *bus = NULL;

	hid = hid_allocate_device();
	if (!IS_ERR_OR_NULL(hid)) {
		bus = hid->dev.bus;
		hid_destroy_device(hid);
	} else {
		bus = ERR_PTR(PTR_ERR(hid));
	}

	return bus;
}

/* hid_match_id() is not exported */
static bool appletb_usb_hack_hid_match_id(struct hid_device *hdev,
					  const struct hid_device_id *id)
{
	for (; id->bus; id++) {
		if (id->bus == hdev->bus &&
		    id->vendor == hdev->vendor &&
		    id->product == hdev->product)
			return true;
	}

	return false;
}

static int appletb_hid_bus_changed(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	struct device *dev = data;
	struct hid_device *hdev;
	struct usb_interface *intf;

	pr_debug("HID device changed: action=%lu, dev=%s\n", action,
		 dev_name(dev));

	hdev = to_hid_device(dev);
	if (!appletb_usb_hack_hid_match_id(hdev, appletb_touchbar_devices))
		return NOTIFY_DONE;

	intf = to_usb_interface(hdev->dev.parent);
	if (intf->cur_altsetting->desc.bInterfaceNumber != 2)
		return NOTIFY_DONE;

	switch (action) {
	case BUS_NOTIFY_ADD_DEVICE:
		pr_info("Touchbar usb device added; dev=%s\n", dev_name(dev));

		get_device(&hdev->dev);
		appletb_usb_hack_reg_data.hdev = hdev;
		schedule_work(&appletb_usb_hack_reg_data.work);

		return NOTIFY_OK;

	default:
		break;
	}

	return NOTIFY_DONE;
}

static int appletb_hid_bus_dev_iter(struct device *dev, void *data)
{
	appletb_hid_bus_changed(NULL, BUS_NOTIFY_ADD_DEVICE, dev);
	return 0;
}

static struct notifier_block appletb_hid_bus_notifier = {
	.notifier_call = appletb_hid_bus_changed,
};

static int __init appletb_init(void)
{
	int ret;
	struct bus_type *hid_bus;

	INIT_WORK(&appletb_usb_hack_reg_data.work,
		  appletb_usb_hack_reg_hid_driver);

	ret = hid_register_driver(&appletb_driver);
	if (ret) {
		pr_err("Error registering hid driver: %d\n", ret);
		return ret;
	}

	hid_bus = appletb_usb_hack_get_usb_bus();
	if (IS_ERR_OR_NULL(hid_bus)) {
		ret = PTR_ERR(hid_bus);
		pr_err("Error getting hid bus: %d\n", ret);
		goto unregister_hid_driver;
	}

	ret = bus_register_notifier(hid_bus, &appletb_hid_bus_notifier);
	if (ret) {
		pr_err("Error registering hid bus notifier: %d\n", ret);
		goto unregister_hid_driver;
	}

	bus_for_each_dev(hid_bus, NULL, NULL, appletb_hid_bus_dev_iter);

	return 0;

unregister_hid_driver:
	hid_unregister_driver(&appletb_driver);

	return ret;
}

static void __exit appletb_exit(void)
{
	struct bus_type *hid_bus;

	hid_bus = appletb_usb_hack_get_usb_bus();
	if (!IS_ERR_OR_NULL(hid_bus))
		bus_unregister_notifier(hid_bus, &appletb_hid_bus_notifier);
	else
		pr_err("Error getting hid bus: %ld\n", PTR_ERR(hid_bus));

	hid_unregister_driver(&appletb_driver);
}

module_init(appletb_init);
module_exit(appletb_exit);

#endif /* KERNEL_VERSION >= 4.16 */

MODULE_AUTHOR("Ronald Tschalär");
MODULE_DESCRIPTION("MacBookPro touchbar driver");
MODULE_LICENSE("GPL");
