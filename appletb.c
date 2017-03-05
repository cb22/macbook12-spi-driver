/**
 * This is a basic driver to enable the touchbar on a MacBookPro13,2 and
 * MacBookPro13,3. The touchbar shows up as two HID interfaces on the iBridge
 * USB device. The first interface appears to provide for some simple, basic
 * modes including special-keys and function-keys; the second interface is
 * more elaborate and allows custom configurations. This driver currently
 * just interacts with the first one.
 */

#define pr_fmt(fmt) "appletb: " fmt

#include <linux/module.h>
#include <linux/input.h>
#include <linux/ktime.h>
#include <linux/hid.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>


#define USB_ID_VENDOR_APPLE	0x05ac
#define USB_ID_PRODUCT_IBRIDGE	0x8600

#define MAX_TB_KEYS		13	/* ESC, F1-F12 */

#define APPLETB_CMD_MODE_ESC	0
#define APPLETB_CMD_MODE_FN	1
#define APPLETB_CMD_MODE_SPCL	2
#define APPLETB_CMD_MODE_OFF	3
#define APPLETB_CMD_MODE_NONE	255

#define APPLETB_FN_MODE_FKEYS	0
#define APPLETB_FN_MODE_NORM	1
#define APPLETB_FN_MODE_INV	2
#define APPLETB_FN_MODE_SPCL	3
#define APPLETB_FN_MODE_MAX	APPLETB_FN_MODE_SPCL

#define APPLETB_DEVID_TOUCHBAR	0
#define APPLETB_DEVID_KEYBOARD	1
#define APPLETB_DEVID_TOUCHPAD	2


static int appletb_tb_def_idle_timeout = 5*60;
module_param_named(default_idle_timeout, appletb_tb_def_idle_timeout, int, 0444);
MODULE_PARM_DESC(idle_timeout, "Default touchbar idle timeout (in seconds); "
			       "0 disables touchbar, -1 disables timeout");

static int appletb_tb_def_fn_mode = APPLETB_FN_MODE_NORM;
module_param_named(default_fn_mode, appletb_tb_def_fn_mode, int, 0444);
MODULE_PARM_DESC(fn_mode, "Default FN key mode: 0 = f-keys only, 1 = fn key "
			  "switches from special to f-keys, 2 = inverse of 1, "
			  "3 = special keys only");


static ssize_t idle_timeout_show(struct device *dev, struct device_attribute *attr,
				 char *buf);
static ssize_t idle_timeout_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t size);
static DEVICE_ATTR_RW(idle_timeout);

static ssize_t fn_mode_show(struct device *dev, struct device_attribute *attr,
				 char *buf);
static ssize_t fn_mode_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t size);
static DEVICE_ATTR_RW(fn_mode);

static struct attribute *appletb_attrs[] = {
	&dev_attr_idle_timeout.attr,
	&dev_attr_fn_mode.attr,
	NULL,
};

static const struct attribute_group appletb_attr_group = {
	.attrs = appletb_attrs,
};


struct appletb_data {
	struct usb_interface	*tb_usb_iface;
	unsigned int		tb_usb_epnum;
	unsigned int		tb_usb_ifnum;

	struct input_handler	inp_handler;
	struct input_handle	kbd_handle;
	struct input_handle	tpd_handle;

	bool			last_tb_keys_pressed[MAX_TB_KEYS];
	bool			last_fn_pressed;

	ktime_t			last_event_time;

	unsigned char		cur_tb_mode;
	unsigned char		pnd_tb_mode;
	bool			tb_autopm_off;
	struct delayed_work	tb_mode_work;
	spinlock_t		tb_mode_lock;

	int			idle_timeout;
	int			fn_mode;
};

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

static int appletb_send_usb_ctrl_req(struct usb_interface *iface,
				     unsigned int ep, __u8 request,
				     __u8 requesttype, __u16 value, __u16 index,
				     void *data, __u16 size)
{
	void *buffer;
	struct usb_device *dev = interface_to_usbdev(iface);
	int rc;

	buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	memcpy(buffer, data, size);

	rc = usb_control_msg(dev, usb_sndctrlpipe(dev, ep), request,
			     requesttype, value, index, buffer, size, 2000);

	kfree(buffer);

	return likely(rc > 0) ? 0 : rc;
}

static int appletb_set_tb_mode(struct appletb_data *tb_data, unsigned char mode)
{
	int rc;

	if (!tb_data->tb_usb_iface)
		return -1;

	if (mode != APPLETB_CMD_MODE_OFF &&
	    tb_data->cur_tb_mode == APPLETB_CMD_MODE_OFF) {
		rc = usb_autopm_get_interface(tb_data->tb_usb_iface);
		if (likely(rc == 0))
			tb_data->tb_autopm_off = true;
		else
			pr_err("Failed to disable auto-pm on touchbar device "
			       "(%d)\n", rc);
	}

	rc = appletb_send_usb_ctrl_req(tb_data->tb_usb_iface,
				       tb_data->tb_usb_epnum,
				       USB_REQ_SET_CONFIGURATION,
				       USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
				       0x0302, tb_data->tb_usb_ifnum, &mode, 1);
	if (unlikely(rc < 0))
		pr_err("Failed to set touchbar mode to %u (%d)\n", mode, rc);

	if (mode == APPLETB_CMD_MODE_OFF &&
	    tb_data->cur_tb_mode != APPLETB_CMD_MODE_OFF) {
		if (tb_data->tb_autopm_off) {
			usb_autopm_put_interface(tb_data->tb_usb_iface);
			tb_data->tb_autopm_off = false;
		}
	}

	return rc;
}

static bool appletb_any_tb_key_pressed(struct appletb_data *tb_data)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(tb_data->last_tb_keys_pressed); idx++) {
		if (tb_data->last_tb_keys_pressed[idx])
			return true;
	}

	return false;
}

static void appletb_set_tb_mode_worker(struct work_struct *work)
{
	struct appletb_data *tb_data =
		container_of(work, struct appletb_data, tb_mode_work.work);
	s64 time_left;
	unsigned char pending_mode;
	bool any_tb_key_pressed, need_reschedule;
	int rc1 = 1;

	/* get state */
	spin_lock(&tb_data->tb_mode_lock);

	time_left = tb_data->idle_timeout -
		ktime_ms_delta(ktime_get(), tb_data->last_event_time) / 1000;

	pending_mode = tb_data->pnd_tb_mode;

	any_tb_key_pressed = appletb_any_tb_key_pressed(tb_data);

	spin_unlock(&tb_data->tb_mode_lock);

	/* handle explicit mode-change request */
	if (pending_mode != APPLETB_CMD_MODE_NONE) {
		rc1 = appletb_set_tb_mode(tb_data, pending_mode);
		time_left = tb_data->idle_timeout;
	}

	/* update pending states */
	need_reschedule = false;

	spin_lock(&tb_data->tb_mode_lock);

	if (rc1 == 0) {
		tb_data->cur_tb_mode = pending_mode;

		if (tb_data->pnd_tb_mode == pending_mode)
			tb_data->pnd_tb_mode = APPLETB_CMD_MODE_NONE;
		else
			need_reschedule = true;
	}

	spin_unlock(&tb_data->tb_mode_lock);

	if (need_reschedule) {
		schedule_delayed_work(&tb_data->tb_mode_work, 0);
		return;
	}

	/* if no idle timeout, we're done */
	if (tb_data->idle_timeout <= 0)
		return;

	/* manage idle timeout */
	if (time_left > 0) {
		/* we fired too soon or had a mode-change- re-schedule */
		schedule_delayed_work(&tb_data->tb_mode_work,
				      msecs_to_jiffies(time_left * 1000));
	} else if (any_tb_key_pressed) {
		/* keys are still pressed - re-schedule */
		schedule_delayed_work(&tb_data->tb_mode_work,
			msecs_to_jiffies(tb_data->idle_timeout * 1000));
	} else {
		/* idle timeout reached */
		appletb_set_tb_mode(tb_data, APPLETB_CMD_MODE_OFF);
	}
}

static u16 appletb_fn_to_special(u16 code)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(appletb_fn_codes); idx++) {
		if (appletb_fn_codes[idx].from == code) {
			return appletb_fn_codes[idx].to;
		}
	}

	return 0;
}

static unsigned char appletb_get_cur_tb_mode(struct appletb_data *tb_data)
{
	return tb_data->pnd_tb_mode != APPLETB_CMD_MODE_NONE ?
				tb_data->pnd_tb_mode : tb_data->cur_tb_mode;
}

static unsigned char appletb_get_fn_tb_mode(struct appletb_data *tb_data)
{
	switch (tb_data->fn_mode) {
	case APPLETB_FN_MODE_FKEYS:
		return APPLETB_CMD_MODE_FN;

	case APPLETB_FN_MODE_SPCL:
		return APPLETB_CMD_MODE_SPCL;

	case APPLETB_FN_MODE_INV:
		return (tb_data->last_fn_pressed) ? APPLETB_CMD_MODE_SPCL :
						    APPLETB_CMD_MODE_FN;

	case APPLETB_FN_MODE_NORM:
	default:
		return (tb_data->last_fn_pressed) ? APPLETB_CMD_MODE_FN :
						    APPLETB_CMD_MODE_SPCL;
	}
}

static void update_touchbar_mode(struct appletb_data *tb_data)
{
	unsigned long flags;

	spin_lock_irqsave(&tb_data->tb_mode_lock, flags);

	if (tb_data->idle_timeout < 0 &&
	    appletb_get_cur_tb_mode(tb_data) == APPLETB_CMD_MODE_OFF)
		tb_data->pnd_tb_mode = appletb_get_fn_tb_mode(tb_data);
	else if (tb_data->idle_timeout == 0)
		tb_data->pnd_tb_mode = APPLETB_CMD_MODE_OFF;

	spin_unlock_irqrestore(&tb_data->tb_mode_lock, flags);

	cancel_delayed_work(&tb_data->tb_mode_work);
	schedule_delayed_work(&tb_data->tb_mode_work, 0);
}

/* Switch touchbar mode when mode is not the desired one and no touchbar keys
 * are pressed.
 */
static void appletb_do_tb_mode_switch(struct appletb_data *tb_data)
{
	unsigned char want_mode;

	if (tb_data->idle_timeout == 0)
		return;

	want_mode = appletb_get_fn_tb_mode(tb_data);

	if (appletb_get_cur_tb_mode(tb_data) != want_mode &&
	    !appletb_any_tb_key_pressed(tb_data)) {
		tb_data->pnd_tb_mode = want_mode;

		cancel_delayed_work(&tb_data->tb_mode_work);
		schedule_delayed_work(&tb_data->tb_mode_work, 0);
	}
}

static ssize_t idle_timeout_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	struct appletb_data *tb_data = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%d\n", tb_data->idle_timeout);
}

static ssize_t idle_timeout_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t size)
{
	struct appletb_data *tb_data = dev_get_drvdata(dev);
	char *end;
	long new;

	new = simple_strtol(buf, &end, 0);
	if (end == buf || new > INT_MAX || new < -1)
		return -EINVAL;

	tb_data->idle_timeout = new;
	update_touchbar_mode(tb_data);

	return size;
}

static ssize_t fn_mode_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct appletb_data *tb_data = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%d\n", tb_data->fn_mode);
}

static ssize_t fn_mode_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t size)
{
	struct appletb_data *tb_data = dev_get_drvdata(dev);
	char *end;
	long new;

	new = simple_strtol(buf, &end, 0);
	if (end == buf || new > APPLETB_FN_MODE_MAX || new < 0)
		return -EINVAL;

	tb_data->fn_mode = new;
	appletb_do_tb_mode_switch(tb_data);

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
	struct appletb_data *tb_data = hid_get_drvdata(hdev);
	unsigned long flags;
	unsigned int new_code;
	int slot;
	int rc = 0;

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

	spin_lock_irqsave(&tb_data->tb_mode_lock, flags);

	/* remember which (untranslated) touchbar keys are pressed */
	if (usage->type == EV_KEY && value != 2)
		tb_data->last_tb_keys_pressed[slot] = value;

	/* remember last time keyboard or touchpad was touched */
	tb_data->last_event_time = ktime_get();

	/* only switch touchbar mode when no touchbar keys are pressed */
	appletb_do_tb_mode_switch(tb_data);

	/* translate special keys */
	if (usage->type == EV_KEY &&
	    (new_code = appletb_fn_to_special(usage->code)) &&
	    appletb_get_cur_tb_mode(tb_data) == APPLETB_CMD_MODE_SPCL) {
		input_event(field->hidinput->input, usage->type, new_code,
			    value);
		rc = 1;
	}

	/* everything else handled normally */
	spin_unlock_irqrestore(&tb_data->tb_mode_lock, flags);

	return rc;
}

static void appletb_inp_event(struct input_handle *handle, unsigned int type,
			      unsigned int code, int value)
{
	struct appletb_data *tb_data = handle->private;
	unsigned long flags;

	spin_lock_irqsave(&tb_data->tb_mode_lock, flags);

	/* remember last state of FN key */
	if (type == EV_KEY && code == KEY_FN && value != 2)
		tb_data->last_fn_pressed = value;

	/* remember last time keyboard or touchpad was touched */
	tb_data->last_event_time = ktime_get();

	/* only switch touchbar mode when no touchbar keys are pressed */
	appletb_do_tb_mode_switch(tb_data);

	spin_unlock_irqrestore(&tb_data->tb_mode_lock, flags);
}

/* Find and save the usb-device associated with the touchbar input device */
static int appletb_get_tb_usb_dev_info(struct appletb_data *tb_data,
				       struct device *dev)
{
	struct usb_interface *iface;

	/* find the usb-interface device */
	if (!dev->bus || strcmp(dev->bus->name, "hid") != 0)
		return -ENXIO;

	dev = dev->parent;
	if (!dev || !dev->bus || strcmp(dev->bus->name, "usb") != 0)
		return -ENXIO;

	iface = to_usb_interface(dev);

	/* extract the info we need from it */
	tb_data->tb_usb_iface = iface;
	tb_data->tb_usb_epnum = 0;
	tb_data->tb_usb_ifnum = iface->cur_altsetting->desc.bInterfaceNumber;

	return 0;
}

static int appletb_inp_connect(struct input_handler *handler,
			       struct input_dev *dev,
			       const struct input_device_id *id)
{
	struct appletb_data *tb_data = handler->private;
	struct input_handle *handle;
	int rc;

	if (id->driver_info == APPLETB_DEVID_KEYBOARD) {
		handle = &tb_data->kbd_handle;
		handle->name = "tbkbd";
	} else if (id->driver_info == APPLETB_DEVID_TOUCHPAD) {
		handle = &tb_data->tpd_handle;
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
	handle->private = tb_data;

	rc = input_register_handle(handle);
	if (rc)
		goto err_free_dev;

	rc = input_open_device(handle);
	if (rc)
		goto err_unregister_handle;

	pr_info("Connected to %s input device\n",
		handle == &tb_data->kbd_handle ? "keyboard" : "touchpad");

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
	struct appletb_data *tb_data = handle->private;

	input_close_device(handle);
	input_unregister_handle(handle);

	input_put_device(handle->dev);
	handle->dev = NULL;

	pr_info("Disconnected from %s input device\n",
		handle == &tb_data->kbd_handle ? "keyboard" : "touchpad");
}

static int appletb_input_configured(struct hid_device *hdev,
				    struct hid_input *hidinput)
{
	int idx;

	/* Add the translated keys to the keybits */
	for (idx = 0; idx < ARRAY_SIZE(appletb_fn_codes); idx++)
		input_set_capability(hidinput->input, EV_KEY,
				     appletb_fn_codes[idx].to);

	return 0;
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

static int appletb_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct appletb_data *tb_data;
	int rc;

	/* Allocate the driver data */
	tb_data = kzalloc(sizeof(*tb_data), GFP_KERNEL);
	if (!tb_data)
		return -ENOMEM;

	/* Initialize structures */
	spin_lock_init(&tb_data->tb_mode_lock);
	INIT_DELAYED_WORK(&tb_data->tb_mode_work, appletb_set_tb_mode_worker);

	/* initialize the usb-device */
	rc = appletb_get_tb_usb_dev_info(tb_data, &hdev->dev);
	if (rc) {
		hid_err(hdev, "Failed to find touchbar device (%d)\n", rc);
		goto free_mem;
	}

	/* initialize the touchbar */
	if (appletb_tb_def_fn_mode >= 0 &&
	    appletb_tb_def_fn_mode <= APPLETB_FN_MODE_MAX)
		tb_data->fn_mode = appletb_tb_def_fn_mode;
	else
		tb_data->fn_mode = APPLETB_FN_MODE_NORM;
	tb_data->idle_timeout = appletb_tb_def_idle_timeout;

	tb_data->cur_tb_mode = APPLETB_CMD_MODE_OFF;
	tb_data->pnd_tb_mode = appletb_get_fn_tb_mode(tb_data);

	update_touchbar_mode(tb_data);

	/* Set up the hid */
	hid_set_drvdata(hdev, tb_data);

	rc = hid_parse(hdev);
	if (rc) {
		hid_err(hdev, "hid parse failed (%d)\n", rc);
		goto cancel_work;
	}

	rc = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (rc) {
		hid_err(hdev, "hw start failed (%d)\n", rc);
		goto cancel_work;
	}

	/* Set up the input handler */
	tb_data->inp_handler.event = appletb_inp_event;
	tb_data->inp_handler.connect = appletb_inp_connect;
	tb_data->inp_handler.disconnect = appletb_inp_disconnect;
	tb_data->inp_handler.name = "appletb";
	tb_data->inp_handler.id_table = appletb_input_devices;
	tb_data->inp_handler.private = tb_data;

	rc = input_register_handler(&tb_data->inp_handler);
	if (rc) {
		hid_err(hdev, "Unabled to register keyboard handler (%d)\n", rc);
		goto stop_hw;
	}

	/* initialize sysfs attributes */
	rc = sysfs_create_group(&hdev->dev.kobj, &appletb_attr_group);
	if (rc) {
		hid_err(hdev, "Failed to create sysfs attributes (%d)\n", rc);
		goto unreg_handler;
	}

	/* done */
	hid_info(hdev, "module probe done.\n");

	return 0;

 unreg_handler:
	input_unregister_handler(&tb_data->inp_handler);
 stop_hw:
	hid_hw_stop(hdev);
 cancel_work:
	cancel_delayed_work_sync(&tb_data->tb_mode_work);
 free_mem:
	kfree(tb_data);

	return rc;
}

static void appletb_remove(struct hid_device *hdev)
{
	struct appletb_data *tb_data = hid_get_drvdata(hdev);

	sysfs_remove_group(&hdev->dev.kobj, &appletb_attr_group);

	input_unregister_handler(&tb_data->inp_handler);

	hid_hw_stop(hdev);

	cancel_delayed_work_sync(&tb_data->tb_mode_work);
	appletb_set_tb_mode(tb_data, APPLETB_CMD_MODE_OFF);

	kfree(tb_data);

	hid_info(hdev, "module remove done.\n");
}

/* TODO: is this necessary? */
static int appletb_resume(struct hid_device *hdev)
{
	struct appletb_data *tb_data = hid_get_drvdata(hdev);

	/* restore touchbar state */
	appletb_set_tb_mode(tb_data, tb_data->cur_tb_mode);

	hid_info(hdev, "device resume done.\n");

	return 0;
}

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
	.resume = appletb_resume,
#endif
};

#ifdef WE_ARE_IN_HID_IGNORE_LIST
module_hid_driver(appletb_driver);

#else /* WE_ARE_IN_HID_IGNORE_LIST */
/*
 * Horrible hack to work around the fact that it's not possible to dynamically
 * be added to hid-core's hid_ignore_list. This means the hid-generic will hid
 * driver always get attached to the touchbar device. This workaround attempts
 * to detect that, release the driver, and trigger our driver instead.
 */

#define APPLETB_TB_SIMPLE_IFNUM 2

static struct {
	struct delayed_work  work;
	struct usb_interface *intf;
	ktime_t start;
} appletb_usb_hack_check_data;

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

static void appletb_usb_hack_check_hid_driver(struct work_struct *work)
{
	struct usb_interface *intf;
	struct hid_device *hid;
	s64 wait_time;
	int rc;

	/* check if we're still active */
	intf = usb_get_intf(appletb_usb_hack_check_data.intf);
	if (!intf)
		return;

	wait_time = ktime_ms_delta(ktime_get(), appletb_usb_hack_check_data.start);

	/* get associated hid device */
	hid = usb_get_intfdata(intf);

	/* wait up to 3s for hid dev and driver to get attached */
	if ((!hid || !hid->driver) && wait_time < 3000) {
		/* try again */
		schedule_delayed_work(&appletb_usb_hack_check_data.work,
				      msecs_to_jiffies(50));
		goto done;
	}

	if (!hid || !hid->driver) {
		pr_warn("No hid driver attached to touchbar in 3s - giving up");
		goto all_done;
	}

	/* check if we got attached */
	if (strcmp(hid->driver->name, appletb_driver.name) == 0)
		goto all_done;

	/* else normally expect hid-generic to be the one attached */
	if (strcmp(hid->driver->name, "hid-generic") != 0) {
		pr_warn("Unexpected hid driver '%s' attached to touchbar",
			hid->driver->name);
	}

	/* detach current driver, and re-register ourselves in order to
	 * trigger attachment. */
	pr_info("releasing current hid driver '%s'\n", hid->driver->name);
	appletb_usb_hack_release_hid_dev(hid);

	hid_unregister_driver(&appletb_driver);
	rc = hid_register_driver(&appletb_driver);
	if (rc)
		pr_err("Error (re)registering touchbar hid driver (%d)", rc);

 all_done:
	appletb_usb_hack_check_data.intf = NULL;
 done:
	usb_put_intf(intf);
}

static int appletb_usb_hack_probe(struct usb_interface *intf,
				  const struct usb_device_id *id)
{
	struct usb_device *udev;

	udev = interface_to_usbdev(intf);
	intf = usb_ifnum_to_if(udev, APPLETB_TB_SIMPLE_IFNUM);

	if (intf && intf != appletb_usb_hack_check_data.intf) {
		appletb_usb_hack_check_data.intf = intf;
		appletb_usb_hack_check_data.start = ktime_get();
		schedule_delayed_work(&appletb_usb_hack_check_data.work,
				      msecs_to_jiffies(50));
	}

	return -ENODEV;
}

static void appletb_usb_hack_disconnect(struct usb_interface *intf)
{
	cancel_delayed_work_sync(&appletb_usb_hack_check_data.work);
	appletb_usb_hack_check_data.intf = NULL;
}

static int appletb_usb_hack_register(struct usb_driver *drv)
{
	int rc;

	INIT_DELAYED_WORK(&appletb_usb_hack_check_data.work,
			  appletb_usb_hack_check_hid_driver);

	rc = hid_register_driver(&appletb_driver);
	if (rc)
		return rc;

	return usb_register(drv);
}

static void appletb_usb_hack_unregister(struct usb_driver *drv)
{
	usb_deregister(drv);
	hid_unregister_driver(&appletb_driver);
}

static const struct usb_device_id appletb_usb_hack_devices[] = {
	{ USB_DEVICE(USB_ID_VENDOR_APPLE, USB_ID_PRODUCT_IBRIDGE) },
	{ },
};
MODULE_DEVICE_TABLE(usb, appletb_usb_hack_devices);

static struct usb_driver appletb_usb_hack_driver = {
	.name = "apple-touchbar-usb-hack",
	.probe = appletb_usb_hack_probe,
	.disconnect = appletb_usb_hack_disconnect,
	.id_table = appletb_usb_hack_devices,
};

module_driver(appletb_usb_hack_driver, appletb_usb_hack_register,
	      appletb_usb_hack_unregister);
#endif /* WE_ARE_IN_HID_IGNORE_LIST */

MODULE_AUTHOR("Ronald Tschalär");
MODULE_DESCRIPTION("MacBookPro touchbar driver");
MODULE_LICENSE("GPL");
