/**
 * The keyboard and touchpad controller on the MacBook8,1, MacBook9,1 and
 * MacBookPro12,1 can be driven either by USB or SPI. However the USB pins
 * are only connected on the MacBookPro12,1, all others need this driver.
 * The interface is selected using ACPI methods:
 *
 * * UIEN ("USB Interface Enable"): If invoked with argument 1, disables SPI
 *   and enables USB. If invoked with argument 0, disables USB.
 * * UIST ("USB Interface Status"): Returns 1 if USB is enabled, 0 otherwise.
 * * SIEN ("SPI Interface Enable"): If invoked with argument 1, disables USB
 *   and enables SPI. If invoked with argument 0, disables SPI.
 * * SIST ("SPI Interface Status"): Returns 1 if SPI is enabled, 0 otherwise.
 * * ISOL: Resets the four GPIO pins used for SPI. Intended to be invoked with
 *   argument 1, then once more with argument 0.
 *
 * UIEN and UIST are only provided on the MacBookPro12,1.
 */

#define pr_fmt(fmt) "applespi: " fmt

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/spinlock.h>
#include <linux/crc16.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/notifier.h>
#include <linux/leds.h>
#if defined(DEBUG) || defined(CONFIG_DYNAMIC_DEBUG)
#include <linux/ktime.h>
#endif

#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input-polldev.h>

#define APPLESPI_PACKET_SIZE    256
#define APPLESPI_STATUS_SIZE    4

#define PACKET_TYPE_READ        0x20
#define PACKET_TYPE_WRITE       0x40
#define PACKET_DEV_KEYB         0x01
#define PACKET_DEV_TPAD         0x02

#define MAX_ROLLOVER 		6

#define MAX_FINGERS		6
#define MAX_FINGER_ORIENTATION	16384

#define MIN_KBD_BL_LEVEL	32
#define MAX_KBD_BL_LEVEL	255
#define KBD_BL_LEVEL_SCALE	1000000
#define KBD_BL_LEVEL_ADJ	\
	((MAX_KBD_BL_LEVEL - MIN_KBD_BL_LEVEL) * KBD_BL_LEVEL_SCALE / 255)

#define DBG_CMD_TP_INI		(1 <<  0)
#define DBG_CMD_BL		(1 <<  1)
#define DBG_CMD_CL		(1 <<  2)
#define DBG_RD_KEYB		(1 <<  8)
#define DBG_RD_TPAD		(1 <<  9)
#define DBG_RD_UNKN		(1 << 10)
#define DBG_RD_IRQ 		(1 << 11)
#define DBG_TP_DIM		(1 << 16)

#define	debug_print(mask, fmt, ...) \
	if (debug & mask) \
		printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#define	debug_print_buffer(mask, fmt, ...) \
	if (debug & mask) \
		print_hex_dump(KERN_DEBUG, pr_fmt(fmt), DUMP_PREFIX_NONE, \
		32, 1, ##__VA_ARGS__, false)

#define APPLE_FLAG_FKEY		0x01

#define SPI_DEV_CHIP_SEL	0	// from DSDT UBUF
#define SPI_RW_CHG_DLY		100	/* from experimentation, in us */

static unsigned int fnmode = 1;
module_param(fnmode, uint, 0644);
MODULE_PARM_DESC(fnmode, "Mode of fn key on Apple keyboards (0 = disabled, "
		"[1] = fkeyslast, 2 = fkeysfirst)");

static unsigned int iso_layout = 0;
module_param(iso_layout, uint, 0644);
MODULE_PARM_DESC(iso_layout, "Enable/Disable hardcoded ISO-layout of the keyboard. "
		"([0] = disabled, 1 = enabled)");

static unsigned int debug = 0;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "Enable/Disable debug logging. This is a bitmask.");


struct keyboard_protocol {
	u8		packet_type;
	u8		device;
	u8		unknown1[9];
	u8		counter;
	u8		unknown2[5];
	u8		modifiers;
	u8		unknown3;
	u8		keys_pressed[6];
	u8		fn_pressed;
	u16		crc_16;
	u8		unused[228];
};

/* trackpad finger structure, le16-aligned */
struct tp_finger {
	__le16 origin;          /* zero when switching track finger */
	__le16 abs_x;           /* absolute x coodinate */
	__le16 abs_y;           /* absolute y coodinate */
	__le16 rel_x;           /* relative x coodinate */
	__le16 rel_y;           /* relative y coodinate */
	__le16 tool_major;      /* tool area, major axis */
	__le16 tool_minor;      /* tool area, minor axis */
	__le16 orientation;     /* 16384 when point, else 15 bit angle */
	__le16 touch_major;     /* touch area, major axis */
	__le16 touch_minor;     /* touch area, minor axis */
	__le16 unused[2];       /* zeros */
	__le16 pressure;        /* pressure on forcetouch touchpad */
	__le16 multi;           /* one finger: varies, more fingers: constant */
	__le16 padding;
} __attribute__((packed,aligned(2)));

struct touchpad_protocol {
	u8			packet_type;
	u8			device;
	u8			unknown1[4];
	u8			number_of_fingers;
	u8			unknown2[4];
	u8			counter;
	u8			unknown3[2];
	u8			number_of_fingers2;
	u8			unknown[2];
	u8			clicked;
	u8			rel_x;
	u8			rel_y;
	u8			unknown4[44];
	struct tp_finger	fingers[MAX_FINGERS];
	u8			unknown5[208];
};

struct appleacpi_spi_registration_info {
	struct class_interface	cif;
	struct acpi_device 	*adev;
	struct spi_device 	*spi;
	struct spi_master	*spi_master;
	struct delayed_work	work;
	struct notifier_block	slave_notifier;
};

struct spi_settings {
	u64	spi_sclk_period;	/* period in ns */
	u64	spi_word_size;   	/* in number of bits */
	u64	spi_bit_order;   	/* 1 = MSB_FIRST, 0 = LSB_FIRST */
	u64	spi_spo;        	/* clock polarity: 0 = low, 1 = high */
	u64	spi_sph;		/* clock phase: 0 = first, 1 = second */
	u64	spi_cs_delay;    	/* cs-to-clk delay in us */
	u64	reset_a2r_usec;  	/* active-to-receive delay? */
	u64	reset_rec_usec;  	/* ? (cur val: 10) */
};

struct applespi_acpi_map_entry {
	char *name;
	size_t field_offset;
};

static const struct applespi_acpi_map_entry applespi_spi_settings_map[] = {
	{ "spiSclkPeriod", offsetof(struct spi_settings, spi_sclk_period) },
	{ "spiWordSize",   offsetof(struct spi_settings, spi_word_size) },
	{ "spiBitOrder",   offsetof(struct spi_settings, spi_bit_order) },
	{ "spiSPO",        offsetof(struct spi_settings, spi_spo) },
	{ "spiSPH",        offsetof(struct spi_settings, spi_sph) },
	{ "spiCSDelay",    offsetof(struct spi_settings, spi_cs_delay) },
	{ "resetA2RUsec",  offsetof(struct spi_settings, reset_a2r_usec) },
	{ "resetRecUsec",  offsetof(struct spi_settings, reset_rec_usec) },
};

struct applespi_tp_info {
	int	x_min;
	int	x_max;
	int	y_min;
	int	y_max;
};

struct applespi_data {
	struct spi_device		*spi;
	struct spi_settings		spi_settings;
	struct input_dev		*keyboard_input_dev;
	struct input_dev		*touchpad_input_dev;

	u8				*tx_buffer;
	u8				*tx_status;
	u8				*rx_buffer;

	const struct applespi_tp_info	*tp_info;

	u8				last_keys_pressed[MAX_ROLLOVER];
	u8				last_keys_fn_pressed[MAX_ROLLOVER];
	u8				last_fn_pressed;
	struct input_mt_pos		pos[MAX_FINGERS];
	int				slots[MAX_FINGERS];
	acpi_handle			handle;
	int				gpe;
	acpi_handle			sien;
	acpi_handle			sist;

	struct spi_transfer		dl_t;
	struct spi_transfer		rd_t;
	struct spi_message		rd_m;

	struct spi_transfer		wd_t;
	struct spi_transfer		wr_t;
	struct spi_transfer		st_t;
	struct spi_message		wr_m;

	int				init_cmd_idx;
	bool				want_cl_led_on;
	bool				have_cl_led_on;
	unsigned			want_bl_level;
	unsigned			have_bl_level;
	unsigned			cmd_msg_cntr;
	spinlock_t			cmd_msg_lock;
	bool				cmd_msg_queued;
	unsigned			cmd_log_mask;

	struct led_classdev		backlight_info;

	bool				drain;
	wait_queue_head_t		drain_complete;
	bool				read_active;
	bool				write_active;
};

static const unsigned char applespi_scancodes[] = {
	0,  0,  0,  0,
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
	KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
	KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS,
	KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, 0,
	KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT, KEY_SLASH,
	KEY_CAPSLOCK,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
	KEY_F10, KEY_F11, KEY_F12, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_102ND,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_RO, 0, KEY_YEN, 0, 0, 0, 0, 0,
	0, KEY_KATAKANAHIRAGANA, KEY_MUHENKAN
};

static const unsigned char applespi_controlcodes[] = {
	KEY_LEFTCTRL,
	KEY_LEFTSHIFT,
	KEY_LEFTALT,
	KEY_LEFTMETA,
	0,
	KEY_RIGHTSHIFT,
	KEY_RIGHTALT,
	KEY_RIGHTMETA
};

struct applespi_key_translation {
	u16 from;
	u16 to;
	u8 flags;
};

static const struct applespi_key_translation applespi_fn_codes[] = {
	{ KEY_BACKSPACE, KEY_DELETE },
	{ KEY_ENTER,	KEY_INSERT },
	{ KEY_F1,	KEY_BRIGHTNESSDOWN, APPLE_FLAG_FKEY },
	{ KEY_F2,	KEY_BRIGHTNESSUP,   APPLE_FLAG_FKEY },
	{ KEY_F3,	KEY_SCALE,          APPLE_FLAG_FKEY },
	{ KEY_F4,	KEY_DASHBOARD,      APPLE_FLAG_FKEY },
	{ KEY_F5,	KEY_KBDILLUMDOWN,   APPLE_FLAG_FKEY },
	{ KEY_F6,	KEY_KBDILLUMUP,     APPLE_FLAG_FKEY },
	{ KEY_F7,	KEY_PREVIOUSSONG,   APPLE_FLAG_FKEY },
	{ KEY_F8,	KEY_PLAYPAUSE,      APPLE_FLAG_FKEY },
	{ KEY_F9,	KEY_NEXTSONG,       APPLE_FLAG_FKEY },
	{ KEY_F10,	KEY_MUTE,           APPLE_FLAG_FKEY },
	{ KEY_F11,	KEY_VOLUMEDOWN,     APPLE_FLAG_FKEY },
	{ KEY_F12,	KEY_VOLUMEUP,       APPLE_FLAG_FKEY },
	{ KEY_RIGHT,	KEY_END },
	{ KEY_LEFT,	KEY_HOME },
	{ KEY_DOWN,	KEY_PAGEDOWN },
	{ KEY_UP,	KEY_PAGEUP },
	{ },
};

static const struct applespi_key_translation apple_iso_keyboard[] = {
	{ KEY_GRAVE,	KEY_102ND },
	{ KEY_102ND,	KEY_GRAVE },
	{ },
};

static u8 *acpi_dsm_uuid = "a0b5b7c6-1318-441c-b0c9-fe695eaf949b";

static struct applespi_tp_info applespi_macbookpro131_info = { -6243, 6749, -170, 7685 };
static struct applespi_tp_info applespi_macbookpro133_info = { -7456, 7976, -163, 9283 };
// MacBook8, MacBook9, MacBook10
static struct applespi_tp_info applespi_default_info = { -5087, 5579, -182, 6089 };

static const struct dmi_system_id applespi_touchpad_infos[] = {
	{
		.ident = "Apple MacBookPro13,1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,1")
		},
		.driver_data = &applespi_macbookpro131_info,
	},
	{
		.ident = "Apple MacBookPro13,2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,2")
		},
		.driver_data = &applespi_macbookpro131_info,	// same touchpad
	},
	{
		.ident = "Apple MacBookPro13,3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,3")
		},
		.driver_data = &applespi_macbookpro133_info,
	},
	{
		.ident = "Apple MacBookPro14,1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro14,1")
		},
		.driver_data = &applespi_macbookpro131_info,
	},
	{
		.ident = "Apple MacBookPro14,2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro14,2")
		},
		.driver_data = &applespi_macbookpro131_info,	// same touchpad
	},
	{
		.ident = "Apple MacBookPro14,3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro14,3")
		},
		.driver_data = &applespi_macbookpro133_info,
	},
	{
		.ident = "Apple Generic MacBook(Pro)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
		},
		.driver_data = &applespi_default_info,
	},
};

u8 *applespi_init_commands[] = {
	"\x40\x02\x00\x00\x00\x00\x0C\x00\x52\x02\x00\x00\x02\x00\x02\x00\x02\x01\x7B\x11\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x23\xAB",
};

u8 *applespi_caps_lock_led_cmd = "\x40\x01\x00\x00\x00\x00\x0C\x00\x51\x01\x00\x00\x02\x00\x02\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x66\x6a";

u8 *applespi_kbd_led_cmd = "\x40\x01\x00\x00\x00\x00\x10\x00\x51\xB0\x00\x00\x06\x00\x06\x00\xB0\x01\x3E\x00\xF4\x01\x96\xC5\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x3E\x59";

static const char *
applespi_debug_facility(unsigned log_mask)
{
	switch (log_mask) {
	case DBG_CMD_TP_INI:
		return "Touchpad Initialization";
	case DBG_CMD_BL:
		return "Backlight Command";
	case DBG_CMD_CL:
		return "Caps-Lock Command";
	case DBG_RD_KEYB:
		return "Keyboard Event";
	case DBG_RD_TPAD:
		return "Touchpad Event";
	case DBG_RD_UNKN:
		return "Unknown Event";
	case DBG_RD_IRQ:
		return "Interrupt Request";
	case DBG_TP_DIM:
		return "Touchpad Dimensions";
	default:
		return "-Unknown-";
	}
}

static void
applespi_setup_read_txfr(struct applespi_data *applespi,
			 struct spi_transfer *dl_t, struct spi_transfer *rd_t)
{
	memset(dl_t, 0, sizeof *dl_t);
	memset(rd_t, 0, sizeof *rd_t);

	dl_t->delay_usecs = applespi->spi_settings.spi_cs_delay;

	rd_t->rx_buf = applespi->rx_buffer;
	rd_t->len = APPLESPI_PACKET_SIZE;
}

static void
applespi_setup_write_txfr(struct applespi_data *applespi,
			  struct spi_transfer *dl_t, struct spi_transfer *wr_t,
			  struct spi_transfer *st_t)
{
	memset(dl_t, 0, sizeof *dl_t);
	memset(wr_t, 0, sizeof *wr_t);
	memset(st_t, 0, sizeof *st_t);

	dl_t->delay_usecs = applespi->spi_settings.spi_cs_delay;

	wr_t->tx_buf = applespi->tx_buffer;
	wr_t->len = APPLESPI_PACKET_SIZE;
	wr_t->delay_usecs = SPI_RW_CHG_DLY;

	st_t->rx_buf = applespi->tx_status;
	st_t->len = APPLESPI_STATUS_SIZE;
}

static void
applespi_setup_spi_message(struct spi_message *message, int num_txfrs, ...)
{
	va_list txfrs;

	spi_message_init(message);

	va_start(txfrs, num_txfrs);
	while (num_txfrs-- > 0)
		spi_message_add_tail(va_arg(txfrs, struct spi_transfer *),
				     message);
	va_end(txfrs);
}

static int
applespi_async(struct applespi_data *applespi, struct spi_message *message,
	       void (*complete)(void *))
{
	message->complete = complete;
	message->context = applespi;

	return spi_async(applespi->spi, message);
}

static inline bool
applespi_check_write_status(struct applespi_data *applespi, int sts)
{
	static u8 sts_ok[] = { 0xac, 0x27, 0x68, 0xd5 };
	bool ret = true;

	if (sts < 0) {
		ret = false;
		pr_warn("Error writing to device: %d\n", sts);
	} else if (memcmp(applespi->tx_status, sts_ok, APPLESPI_STATUS_SIZE) != 0) {
		ret = false;
		pr_warn("Error writing to device: %x %x %x %x\n",
			applespi->tx_status[0], applespi->tx_status[1],
			applespi->tx_status[2], applespi->tx_status[3]);
	}

	return ret;
}

static int applespi_find_settings_field(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(applespi_spi_settings_map); i++) {
		if (strcmp(applespi_spi_settings_map[i].name, name) == 0)
			return applespi_spi_settings_map[i].field_offset;
	}

	return -1;
}

static int applespi_get_spi_settings(acpi_handle handle,
				     struct spi_settings *settings)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
	guid_t guid, *uuid = &guid;
#else
	u8 uuid[16];
#endif
	union acpi_object *spi_info;
	union acpi_object name;
	union acpi_object value;
	int i;
	int field_off;
	u64 *field;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
	guid_parse(acpi_dsm_uuid, uuid);
#else
	acpi_str_to_uuid(acpi_dsm_uuid, uuid);
#endif

	spi_info = acpi_evaluate_dsm(handle, uuid, 1, 1, NULL);
	if (!spi_info) {
		pr_err("Failed to get SPI info from _DSM method\n");
		return -ENODEV;
	}
	if (spi_info->type != ACPI_TYPE_PACKAGE) {
		pr_err("Unexpected data returned from SPI _DSM method: "
		       "type=%d\n", spi_info->type);
		ACPI_FREE(spi_info);
		return -ENODEV;
	}

	/* The data is stored in pairs of items, first a string containing
	 * the name of the item, followed by an 8-byte buffer containing the
	 * value in little-endian.
	 */
	for (i = 0; i < spi_info->package.count - 1; i += 2) {
		name = spi_info->package.elements[i];
		value = spi_info->package.elements[i + 1];

		if (!(name.type == ACPI_TYPE_STRING &&
		      value.type == ACPI_TYPE_BUFFER &&
		      value.buffer.length == 8)) {
			pr_warn("Unexpected data returned from SPI _DSM method:"
			        " name.type=%d, value.type=%d\n", name.type,
				value.type);
			continue;
		}

		field_off = applespi_find_settings_field(name.string.pointer);
		if (field_off < 0) {
			pr_debug("Skipping unknown SPI setting '%s'\n",
				 name.string.pointer);
			continue;
		}

		field = (u64 *) ((char *) settings + field_off);
		*field = le64_to_cpu(*((__le64 *) value.buffer.pointer));
	}
	ACPI_FREE(spi_info);

	return 0;
}

static int applespi_setup_spi(struct applespi_data *applespi)
{
	int sts;

	sts = applespi_get_spi_settings(applespi->handle,
					&applespi->spi_settings);
	if (sts)
		return sts;

	spin_lock_init(&applespi->cmd_msg_lock);
	init_waitqueue_head(&applespi->drain_complete);

	return 0;
}

static int applespi_enable_spi(struct applespi_data *applespi)
{
	int result;
	long long unsigned int spi_status;

	/* Check if SPI is already enabled, so we can skip the delay below */
	result = acpi_evaluate_integer(applespi->sist, NULL, NULL, &spi_status);
	if (ACPI_SUCCESS(result) && spi_status)
		return 0;

	/* SIEN(1) will enable SPI communication */
	result = acpi_execute_simple_method(applespi->sien, NULL, 1);
	if (ACPI_FAILURE(result)) {
		pr_err("SIEN failed: %s\n", acpi_format_exception(result));
		return -ENODEV;
	}

	/*
	 * Allow the SPI interface to come up before returning. Without this
	 * delay, the SPI commands to enable multitouch mode may not reach
	 * the trackpad controller, causing pointer movement to break upon
	 * resume from sleep.
	 */
	msleep(50);

	return 0;
}

static int applespi_send_cmd_msg(struct applespi_data *applespi);

static void applespi_cmd_msg_complete(struct applespi_data *applespi)
{
	unsigned long flags;

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	applespi->cmd_msg_queued = false;
	applespi_send_cmd_msg(applespi);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);
}

static void applespi_async_write_complete(void *context)
{
	struct applespi_data *applespi = context;

	debug_print(applespi->cmd_log_mask, "--- %s ---------------------------\n",
		    applespi_debug_facility(applespi->cmd_log_mask));
	debug_print_buffer(applespi->cmd_log_mask, "write  ", applespi->tx_buffer,
			   APPLESPI_PACKET_SIZE);
	debug_print_buffer(applespi->cmd_log_mask, "status ", applespi->tx_status,
			   APPLESPI_STATUS_SIZE);

	if (!applespi_check_write_status(applespi, applespi->wr_m.status))
		applespi_cmd_msg_complete(applespi);
}

static int
applespi_send_cmd_msg(struct applespi_data *applespi)
{
	u16 crc;
	int sts;

	/* check if draining */
	if (applespi->drain)
		return 0;

	/* check whether send is in progress */
	if (applespi->cmd_msg_queued)
		return 0;

	/* are we processing init commands? */
	if (applespi->init_cmd_idx >= 0) {
		memcpy(applespi->tx_buffer,
		       applespi_init_commands[applespi->init_cmd_idx],
		       APPLESPI_PACKET_SIZE);

		applespi->init_cmd_idx++;
		if (applespi->init_cmd_idx >= ARRAY_SIZE(applespi_init_commands))
			applespi->init_cmd_idx = -1;

		applespi->cmd_log_mask = DBG_CMD_TP_INI;

	/* do we need caps-lock command? */
	} else if (applespi->want_cl_led_on != applespi->have_cl_led_on) {
		applespi->have_cl_led_on = applespi->want_cl_led_on;
		applespi->cmd_log_mask = DBG_CMD_CL;

		/* build led command buffer */
		memcpy(applespi->tx_buffer, applespi_caps_lock_led_cmd,
		       APPLESPI_PACKET_SIZE);

		applespi->tx_buffer[11] = applespi->cmd_msg_cntr++ & 0xff;
		applespi->tx_buffer[17] = applespi->have_cl_led_on ? 2 : 0;

		crc = crc16(0, applespi->tx_buffer + 8, 10);
		applespi->tx_buffer[18] = crc & 0xff;
		applespi->tx_buffer[19] = crc >> 8;

	/* do we need backlight command? */
	} else if (applespi->want_bl_level != applespi->have_bl_level) {
		applespi->have_bl_level = applespi->want_bl_level;
		applespi->cmd_log_mask = DBG_CMD_BL;

		/* build command buffer */
		memcpy(applespi->tx_buffer, applespi_kbd_led_cmd,
		       APPLESPI_PACKET_SIZE);

		applespi->tx_buffer[11] = applespi->cmd_msg_cntr++ & 0xff;

		applespi->tx_buffer[18] = applespi->have_bl_level & 0xff;
		applespi->tx_buffer[19] = applespi->have_bl_level >> 8;

		if (applespi->have_bl_level > 0) {
			applespi->tx_buffer[20] = 0xF4;
			applespi->tx_buffer[21] = 0x01;
		} else {
			applespi->tx_buffer[20] = 0x01;
			applespi->tx_buffer[21] = 0x00;
		}

		crc = crc16(0, applespi->tx_buffer + 8, 14);
		applespi->tx_buffer[22] = crc & 0xff;
		applespi->tx_buffer[23] = crc >> 8;

	/* everything's up-to-date */
	} else {
		return 0;
	}

	/* send command */
	applespi_setup_write_txfr(applespi, &applespi->wd_t, &applespi->wr_t,
				  &applespi->st_t);
	applespi_setup_spi_message(&applespi->wr_m, 3, &applespi->wd_t,
				   &applespi->wr_t, &applespi->st_t);

	sts = applespi_async(applespi, &applespi->wr_m,
			     applespi_async_write_complete);

	if (sts != 0) {
		pr_warn("Error queueing async write to device: %d\n", sts);
	} else {
		applespi->cmd_msg_queued = true;
		applespi->write_active = true;
	}


	return sts;
}

static void applespi_init(struct applespi_data *applespi)
{
	unsigned long flags;

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	applespi->init_cmd_idx = 0;
	applespi_send_cmd_msg(applespi);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);
}

static int
applespi_set_capsl_led(struct applespi_data *applespi, bool capslock_on)
{
	unsigned long flags;
	int sts;

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	applespi->want_cl_led_on = capslock_on;
	sts = applespi_send_cmd_msg(applespi);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	return sts;
}

static void
applespi_set_bl_level(struct led_classdev *led_cdev, enum led_brightness value)

{
	struct applespi_data *applespi =
		container_of(led_cdev, struct applespi_data, backlight_info);
	unsigned long flags;
	int sts;

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	if (value == 0)
		applespi->want_bl_level = value;
	else
		applespi->want_bl_level = (unsigned)
			((value * KBD_BL_LEVEL_ADJ) / KBD_BL_LEVEL_SCALE +
			 MIN_KBD_BL_LEVEL);

	sts = applespi_send_cmd_msg(applespi);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);
}

static int
applespi_event(struct input_dev *dev, unsigned int type, unsigned int code,
	       int value)
{
	struct applespi_data *applespi = input_get_drvdata(dev);

	switch (type) {

	case EV_LED:
		applespi_set_capsl_led(applespi, !!test_bit(LED_CAPSL, dev->led));
		return 0;
	}

	return -1;
}

/* Lifted from the BCM5974 driver */
/* convert 16-bit little endian to signed integer */
static inline int raw2int(__le16 x)
{
	return (signed short)le16_to_cpu(x);
}

static void report_finger_data(struct input_dev *input, int slot,
			       const struct input_mt_pos *pos,
			       const struct tp_finger *f)
{
	input_mt_slot(input, slot);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, true);

	input_report_abs(input, ABS_MT_TOUCH_MAJOR,
			 raw2int(f->touch_major) << 1);
	input_report_abs(input, ABS_MT_TOUCH_MINOR,
			 raw2int(f->touch_minor) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MAJOR,
			 raw2int(f->tool_major) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MINOR,
			 raw2int(f->tool_minor) << 1);
	input_report_abs(input, ABS_MT_ORIENTATION,
			 MAX_FINGER_ORIENTATION - raw2int(f->orientation));
	input_report_abs(input, ABS_MT_POSITION_X, pos->x);
	input_report_abs(input, ABS_MT_POSITION_Y, pos->y);
}

static int report_tp_state(struct applespi_data *applespi, struct touchpad_protocol* t)
{
	static int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
	static bool dim_updated = false;
	static ktime_t last_print = 0;

	const struct tp_finger *f;
	struct input_dev *input = applespi->touchpad_input_dev;
	const struct applespi_tp_info *tp_info = applespi->tp_info;
	int i, n;

	n = 0;

	for (i = 0; i < MAX_FINGERS; i++) {
		f = &t->fingers[i];
		if (raw2int(f->touch_major) == 0)
			continue;
		applespi->pos[n].x = raw2int(f->abs_x);
		applespi->pos[n].y = tp_info->y_min + tp_info->y_max - raw2int(f->abs_y);
		n++;

		if (debug & DBG_TP_DIM) {
			#define UPDATE_DIMENSIONS(val, op, last) \
				if (raw2int(val) op last) { \
					last = raw2int(val); \
					dim_updated = true; \
				}

			UPDATE_DIMENSIONS(f->abs_x, <, min_x);
			UPDATE_DIMENSIONS(f->abs_x, >, max_x);
			UPDATE_DIMENSIONS(f->abs_y, <, min_y);
			UPDATE_DIMENSIONS(f->abs_y, >, max_y);
		}
	}

	if (debug & DBG_TP_DIM) {
		if (dim_updated &&
		    ktime_ms_delta(ktime_get(), last_print) > 1000) {
			printk(KERN_DEBUG
			       pr_fmt("New touchpad dimensions: %d %d %d %d\n"),
			       min_x, max_x, min_y, max_y);
			dim_updated = false;
			last_print = ktime_get();
		}
	}

	input_mt_assign_slots(input, applespi->slots, applespi->pos, n, 0);

	for (i = 0; i < n; i++)
		report_finger_data(input, applespi->slots[i],
				   &applespi->pos[i], &t->fingers[i]);

	input_mt_sync_frame(input);
	input_report_key(input, BTN_LEFT, t->clicked);

	input_sync(input);
	return 0;
}

static const struct applespi_key_translation*
applespi_find_translation(const struct applespi_key_translation *table, u16 key)
{
	const struct applespi_key_translation *trans;

	for (trans = table; trans->from; trans++)
		if (trans->from == key)
			return trans;

	return NULL;
}

static unsigned int
applespi_code_to_key(u8 code, int fn_pressed)
{
	unsigned int key = applespi_scancodes[code];

	const struct applespi_key_translation *trans;

	if (fnmode) {
		int do_translate;

		trans = applespi_find_translation(applespi_fn_codes, key);
		if (trans) {
			if (trans->flags & APPLE_FLAG_FKEY)
				do_translate = (fnmode == 2 && fn_pressed) ||
					(fnmode == 1 && !fn_pressed);
			else
				do_translate = fn_pressed;

			if (do_translate)
				key = trans->to;
		}
	}

	if (iso_layout) {
		trans = applespi_find_translation(apple_iso_keyboard, key);
		if (trans)
			key = trans->to;
	}

	return key;
}

static void
applespi_handle_keyboard_event(struct applespi_data *applespi,
			       struct keyboard_protocol *keyboard_protocol)
{
	int i, j;
	unsigned int key;
	bool still_pressed;

	for (i=0; i<6; i++) {
		still_pressed = false;
		for (j=0; j<6; j++) {
			if (applespi->last_keys_pressed[i] == keyboard_protocol->keys_pressed[j]) {
				still_pressed = true;
				break;
			}
		}

		if (! still_pressed) {
			key = applespi_code_to_key(applespi->last_keys_pressed[i], applespi->last_keys_fn_pressed[i]);
			input_report_key(applespi->keyboard_input_dev, key, 0);
			applespi->last_keys_fn_pressed[i] = 0;
		}
	}

	for (i=0; i<6; i++) {
		if (keyboard_protocol->keys_pressed[i] < ARRAY_SIZE(applespi_scancodes) && keyboard_protocol->keys_pressed[i] > 0) {
			key = applespi_code_to_key(keyboard_protocol->keys_pressed[i], keyboard_protocol->fn_pressed);
			input_report_key(applespi->keyboard_input_dev, key, 1);
			applespi->last_keys_fn_pressed[i] = keyboard_protocol->fn_pressed;
		}
	}

	// Check control keys
	for (i=0; i<8; i++) {
		if (test_bit(i, (long unsigned int *)&keyboard_protocol->modifiers)) {
			input_report_key(applespi->keyboard_input_dev, applespi_controlcodes[i], 1);
		} else {
			input_report_key(applespi->keyboard_input_dev, applespi_controlcodes[i], 0);
		}
	}

	// Check function key
	if (keyboard_protocol->fn_pressed && !applespi->last_fn_pressed) {
		input_report_key(applespi->keyboard_input_dev, KEY_FN, 1);
	} else if (!keyboard_protocol->fn_pressed && applespi->last_fn_pressed) {
		input_report_key(applespi->keyboard_input_dev, KEY_FN, 0);
	}
	applespi->last_fn_pressed = keyboard_protocol->fn_pressed;

	input_sync(applespi->keyboard_input_dev);
	memcpy(&applespi->last_keys_pressed, keyboard_protocol->keys_pressed, sizeof(applespi->last_keys_pressed));
}

static void
applespi_handle_cmd_response(struct applespi_data *applespi,
			     struct keyboard_protocol *keyboard_protocol)
{
	if (keyboard_protocol->device == PACKET_DEV_TPAD &&
	    memcmp(((u8 *) keyboard_protocol) + 8,
		   applespi_init_commands[0] + 8, 4) == 0)
		pr_info("modeswitch done.\n");
}

static void
applespi_got_data(struct applespi_data *applespi)
{
	struct keyboard_protocol *keyboard_protocol;
	unsigned long flags;

	keyboard_protocol = (struct keyboard_protocol*) applespi->rx_buffer;
	if (keyboard_protocol->packet_type == PACKET_TYPE_READ &&
	    keyboard_protocol->device == PACKET_DEV_KEYB) {
		debug_print(DBG_RD_KEYB, "--- %s ---------------------------\n",
			    applespi_debug_facility(DBG_RD_KEYB));
		debug_print_buffer(DBG_RD_KEYB, "read   ", applespi->rx_buffer,
				   APPLESPI_PACKET_SIZE);

		applespi_handle_keyboard_event(applespi, keyboard_protocol);

	} else if (keyboard_protocol->packet_type == PACKET_TYPE_READ &&
		   keyboard_protocol->device == PACKET_DEV_TPAD) {
		debug_print(DBG_RD_TPAD, "--- %s ---------------------------\n",
			    applespi_debug_facility(DBG_RD_TPAD));
		debug_print_buffer(DBG_RD_TPAD, "read   ", applespi->rx_buffer,
				   APPLESPI_PACKET_SIZE);

		report_tp_state(applespi, (struct touchpad_protocol*) keyboard_protocol);

	} else if (keyboard_protocol->packet_type == PACKET_TYPE_WRITE) {
		debug_print(applespi->cmd_log_mask,
			    "--- %s ---------------------------\n",
			    applespi_debug_facility(applespi->cmd_log_mask));
		debug_print_buffer(applespi->cmd_log_mask, "read   ",
				   applespi->rx_buffer, APPLESPI_PACKET_SIZE);

		applespi_handle_cmd_response(applespi, keyboard_protocol);
	} else {
		debug_print(DBG_RD_UNKN, "--- %s ---------------------------\n",
			    applespi_debug_facility(DBG_RD_UNKN));
		debug_print_buffer(DBG_RD_UNKN, "read   ", applespi->rx_buffer,
				   APPLESPI_PACKET_SIZE);
	}

	/* Note: this relies on the fact that we are blocking the processing of
	 * spi messages at this point, i.e. that no further transfers or cs
	 * changes are processed while we delay here.
	 */
	udelay(SPI_RW_CHG_DLY);

	/* handle draining */
	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	applespi->read_active = false;
	if (keyboard_protocol->packet_type == PACKET_TYPE_WRITE)
		applespi->write_active = false;

	if (applespi->drain && !applespi->write_active)
		wake_up_all(&applespi->drain_complete);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	/* notify write complete */
	if (keyboard_protocol->packet_type == PACKET_TYPE_WRITE)
		applespi_cmd_msg_complete(applespi);
}

static void applespi_async_read_complete(void *context)
{
	struct applespi_data *applespi = context;

	if (applespi->rd_m.status < 0)
		pr_warn("Error reading from device: %d\n", applespi->rd_m.status);
	else
		applespi_got_data(applespi);

	acpi_finish_gpe(NULL, applespi->gpe);
}

static u32 applespi_notify(acpi_handle gpe_device, u32 gpe, void *context)
{
	struct applespi_data *applespi = context;
	int sts;
	unsigned long flags;

	debug_print(DBG_RD_IRQ, "--- %s ---------------------------\n",
		    applespi_debug_facility(DBG_RD_IRQ));

	applespi_setup_read_txfr(applespi, &applespi->dl_t, &applespi->rd_t);
	applespi_setup_spi_message(&applespi->rd_m, 2, &applespi->dl_t, &applespi->rd_t);

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	sts = applespi_async(applespi, &applespi->rd_m, applespi_async_read_complete);
	if (sts != 0)
		pr_warn("Error queueing async read to device: %d\n", sts);
	else
		applespi->read_active = true;

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	return ACPI_INTERRUPT_HANDLED;
}

static int applespi_probe(struct spi_device *spi)
{
	struct applespi_data *applespi;
	int result, i;
	long long unsigned int gpe, usb_status;

	/* Check if the USB interface is present and enabled already */
	result = acpi_evaluate_integer(ACPI_HANDLE(&spi->dev), "UIST", NULL, &usb_status);
	if (ACPI_SUCCESS(result) && usb_status) {
		/* Let the USB driver take over instead */
		pr_info("USB interface already enabled\n");
		return -ENODEV;
	}

	/* Allocate driver data */
	applespi = devm_kzalloc(&spi->dev, sizeof(*applespi), GFP_KERNEL);
	if (!applespi)
		return -ENOMEM;

	applespi->spi = spi;
	applespi->handle = ACPI_HANDLE(&spi->dev);

	/* Store the driver data */
	spi_set_drvdata(spi, applespi);

	/* Create our buffers */
	applespi->tx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE, GFP_KERNEL);
	applespi->tx_status = devm_kmalloc(&spi->dev, APPLESPI_STATUS_SIZE, GFP_KERNEL);
	applespi->rx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE, GFP_KERNEL);

	if (!applespi->tx_buffer || !applespi->tx_status || !applespi->rx_buffer)
		return -ENOMEM;

	/* Cache ACPI method handles */
	if (ACPI_FAILURE(acpi_get_handle(applespi->handle, "SIEN", &applespi->sien)) ||
	    ACPI_FAILURE(acpi_get_handle(applespi->handle, "SIST", &applespi->sist))) {
		pr_err("Failed to get required ACPI method handle\n");
		return -ENODEV;
	}

	/* Switch on the SPI interface */
	result = applespi_setup_spi(applespi);
	if (result)
		return result;

	result = applespi_enable_spi(applespi);
	if (result)
		return result;

	/* Set up touchpad dimensions */
	applespi->tp_info = dmi_first_match(applespi_touchpad_infos)->driver_data;

	/* Setup the keyboard input dev */
	applespi->keyboard_input_dev = devm_input_allocate_device(&spi->dev);

	if (!applespi->keyboard_input_dev)
		return -ENOMEM;

	applespi->keyboard_input_dev->name = "Apple SPI Keyboard";
	applespi->keyboard_input_dev->phys = "applespi/input0";
	applespi->keyboard_input_dev->dev.parent = &spi->dev;
	applespi->keyboard_input_dev->id.bustype = BUS_SPI;

	applespi->keyboard_input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_LED) | BIT_MASK(EV_REP);
	applespi->keyboard_input_dev->ledbit[0] = BIT_MASK(LED_CAPSL);

	input_set_drvdata(applespi->keyboard_input_dev, applespi);
	applespi->keyboard_input_dev->event = applespi_event;

	for (i = 0; i<ARRAY_SIZE(applespi_scancodes); i++)
		if (applespi_scancodes[i])
			input_set_capability(applespi->keyboard_input_dev, EV_KEY, applespi_scancodes[i]);

	for (i = 0; i<ARRAY_SIZE(applespi_controlcodes); i++)
		if (applespi_controlcodes[i])
			input_set_capability(applespi->keyboard_input_dev, EV_KEY, applespi_controlcodes[i]);

	for (i = 0; i<ARRAY_SIZE(applespi_fn_codes); i++)
		if (applespi_fn_codes[i].to)
			input_set_capability(applespi->keyboard_input_dev, EV_KEY, applespi_fn_codes[i].to);

	input_set_capability(applespi->keyboard_input_dev, EV_KEY, KEY_FN);

	result = input_register_device(applespi->keyboard_input_dev);
	if (result) {
		pr_err("Unabled to register keyboard input device (%d)\n",
		       result);
		return -ENODEV;
	}

	/* Now, set up the touchpad as a seperate input device */
	applespi->touchpad_input_dev = devm_input_allocate_device(&spi->dev);

	if (!applespi->touchpad_input_dev)
		return -ENOMEM;

	applespi->touchpad_input_dev->name = "Apple SPI Touchpad";
	applespi->touchpad_input_dev->phys = "applespi/input1";
	applespi->touchpad_input_dev->dev.parent = &spi->dev;
	applespi->touchpad_input_dev->id.bustype = BUS_SPI;

	applespi->touchpad_input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);

	__set_bit(EV_KEY, applespi->touchpad_input_dev->evbit);
	__set_bit(EV_ABS, applespi->touchpad_input_dev->evbit);

	__set_bit(BTN_LEFT, applespi->touchpad_input_dev->keybit);

	__set_bit(INPUT_PROP_POINTER, applespi->touchpad_input_dev->propbit);
	__set_bit(INPUT_PROP_BUTTONPAD, applespi->touchpad_input_dev->propbit);

	/* finger touch area */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MAJOR, 0, 2048, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MINOR, 0, 2048, 0, 0);

	/* finger approach area */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MAJOR, 0, 2048, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MINOR, 0, 2048, 0, 0);

	/* finger orientation */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_ORIENTATION, -MAX_FINGER_ORIENTATION, MAX_FINGER_ORIENTATION, 0, 0);

	/* finger position */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_X, applespi->tp_info->x_min, applespi->tp_info->x_max, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_Y, applespi->tp_info->y_min, applespi->tp_info->y_max, 0, 0);

	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_TOOL_FINGER);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_TOUCH);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_LEFT);

	input_mt_init_slots(applespi->touchpad_input_dev, MAX_FINGERS,
		INPUT_MT_POINTER | INPUT_MT_DROP_UNUSED | INPUT_MT_TRACK);

	result = input_register_device(applespi->touchpad_input_dev);
	if (result) {
		pr_err("Unabled to register touchpad input device (%d)\n",
		       result);
		return -ENODEV;
	}

	/*
	 * The applespi device doesn't send interrupts normally (as is described
	 * in its DSDT), but rather seems to use ACPI GPEs.
	 */
	result = acpi_evaluate_integer(applespi->handle, "_GPE", NULL, &gpe);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to obtain GPE for SPI slave device: %s\n",
		       acpi_format_exception(result));
		return -ENODEV;
	}
	applespi->gpe = (int)gpe;

	result = acpi_install_gpe_handler(NULL, applespi->gpe, ACPI_GPE_LEVEL_TRIGGERED, applespi_notify, applespi);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to install GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(result));
		return -ENODEV;
	}

	result = acpi_enable_gpe(NULL, applespi->gpe);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to enable GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(result));
		acpi_remove_gpe_handler(NULL, applespi->gpe, applespi_notify);
		return -ENODEV;
	}

	/* Switch the touchpad into multitouch mode */
	applespi_init(applespi);

	/* set up keyboard-backlight */
	applespi->backlight_info.name            = "spi::kbd_backlight";
	applespi->backlight_info.default_trigger = "kbd-backlight";
	applespi->backlight_info.brightness_set  = applespi_set_bl_level;

	result = devm_led_classdev_register(&spi->dev,
					    &applespi->backlight_info);
	if (result) {
		pr_err("Unable to register keyboard backlight class dev (%d)\n",
		       result);
		/* not fatal */
	}

	/* done */
	pr_info("spi-device probe done: %s\n", dev_name(&spi->dev));

	return 0;
}

static int applespi_remove(struct spi_device *spi)
{
	struct applespi_data *applespi = spi_get_drvdata(spi);
	unsigned long flags;

	/* wait for all outstanding writes to finish */
	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	applespi->drain = true;
	wait_event_lock_irq(applespi->drain_complete, !applespi->write_active,
			    applespi->cmd_msg_lock);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	/* shut things down */
	acpi_disable_gpe(NULL, applespi->gpe);
	acpi_remove_gpe_handler(NULL, applespi->gpe, applespi_notify);

	/* wait for all outstanding reads to finish */
	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	wait_event_lock_irq(applespi->drain_complete, !applespi->read_active,
			    applespi->cmd_msg_lock);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	/* done */
	pr_info("spi-device remove done: %s\n", dev_name(&spi->dev));
	return 0;
}

#ifdef CONFIG_PM
static int applespi_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct applespi_data *applespi = spi_get_drvdata(spi);
	acpi_status status;

	status = acpi_disable_gpe(NULL, applespi->gpe);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to disable GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(status));
	}

	pr_info("spi-device suspend done.\n");
	return 0;
}

static int applespi_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct applespi_data *applespi = spi_get_drvdata(spi);
	acpi_status status;

	status = acpi_enable_gpe(NULL, applespi->gpe);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to re-enable GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(status));
	}

	/* Switch on the SPI interface */
	applespi_enable_spi(applespi);

	/* Switch the touchpad into multitouch mode */
	applespi_init(applespi);

	pr_info("spi-device resume done.\n");

	return 0;
}
#endif

static const struct acpi_device_id applespi_acpi_match[] = {
	{ "APP000D", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, applespi_acpi_match);

static UNIVERSAL_DEV_PM_OPS(applespi_pm_ops, applespi_suspend,
                            applespi_resume, NULL);

static struct spi_driver applespi_driver = {
	.driver		= {
		.name			= "applespi",
		.owner			= THIS_MODULE,

		.acpi_match_table	= ACPI_PTR(applespi_acpi_match),
		.pm			= &applespi_pm_ops,
	},
	.probe		= applespi_probe,
	.remove		= applespi_remove,
};

/*
 * All the following code is to deal with the fact that the _CRS method for
 * the SPI device in the DSDT returns an empty resource, and the real info is
 * available from the _DSM method. So we need to hook into the ACPI device
 * registration and create and register the SPI device ourselves.
 *
 * All of this can be removed and replaced with
 * module_spi_driver(applespi_driver)
 * when the core adds support for this sort of setup.
 */

/*
 * Configure the spi device with the info from the _DSM method.
 */
static int appleacpi_config_spi_dev(struct spi_device *spi,
				    struct acpi_device *adev)
{
	struct spi_settings settings;
	int ret;

	ret = applespi_get_spi_settings(acpi_device_handle(adev), &settings);
	if (ret)
		return ret;

	spi->max_speed_hz = 1000000000 / settings.spi_sclk_period;
	spi->chip_select = SPI_DEV_CHIP_SEL;
	spi->bits_per_word = settings.spi_word_size;

	spi->mode =
		(settings.spi_spo * SPI_CPOL) |
		(settings.spi_sph * SPI_CPHA) |
		(settings.spi_bit_order == 0 ? SPI_LSB_FIRST : 0);

	spi->irq = -1;		// uses GPE

	spi->dev.platform_data = NULL;
	spi->controller_data = NULL;
	spi->controller_state = NULL;

	pr_debug("spi-config: max_speed_hz=%d, chip_select=%d, bits_per_word=%d,"
		 " mode=%x, irq=%d\n", spi->max_speed_hz, spi->chip_select,
		 spi->bits_per_word, spi->mode, spi->irq);

	return 0;
}

static int appleacpi_is_device_registered(struct device *dev, void *data)
{
	struct spi_device *spi = to_spi_device(dev);
	struct spi_master *spi_master = data;

	if (spi->master == spi_master && spi->chip_select == SPI_DEV_CHIP_SEL)
		return -EBUSY;
	return 0;
}

/*
 * Unregister all physical devices devices associated with the acpi device,
 * so that the new SPI device becomes the first physical device for it.
 * Otherwise we don't get properly registered as the driver for the spi
 * device.
 */
static void appleacpi_unregister_phys_devs(struct acpi_device *adev)
{
	struct acpi_device_physical_node *entry;
	struct device *dev;

	while (true) {
		mutex_lock(&adev->physical_node_lock);

		if (list_empty(&adev->physical_node_list)) {
			mutex_unlock(&adev->physical_node_lock);
			break;
		}

		entry = list_first_entry(&adev->physical_node_list,
					 struct acpi_device_physical_node,
					 node);
		dev = get_device(entry->dev);

		mutex_unlock(&adev->physical_node_lock);

		platform_device_unregister(to_platform_device(dev));
		put_device(dev);
	}
}

/*
 * Create the spi device for the keyboard and touchpad and register it with
 * the master spi device.
 */
static int appleacpi_register_spi_device(struct spi_master *spi_master,
					 struct acpi_device *adev)
{
	struct appleacpi_spi_registration_info *reg_info;
	struct spi_device *spi;
	int ret;

	reg_info = acpi_driver_data(adev);

	/* check if an spi device is already registered */
	ret = bus_for_each_dev(&spi_bus_type, NULL, spi_master,
			       appleacpi_is_device_registered);
	if (ret == -EBUSY) {
		pr_info("Spi Device already registered - patched DSDT?\n");
		ret = 0;
		goto release_master;
	} else if (ret) {
		pr_err("Error checking for spi device registered: %d\n", ret);
		goto release_master;
	}

	/* none is; check if acpi device is there */
	if (acpi_bus_get_status(adev) || !adev->status.present) {
		pr_info("ACPI device is not present\n");
		ret = 0;
		goto release_master;
	}

	/*
	 * acpi device is there.
	 *
	 * First unregister any physical devices already associated with this
	 * acpi device (done by acpi_generic_device_attach).
	 * */
	appleacpi_unregister_phys_devs(adev);

	/* create spi device */
	spi = spi_alloc_device(spi_master);
	if (!spi) {
		pr_err("Failed to allocate spi device\n");
		ret = -ENOMEM;
		goto release_master;
	}

	ret = appleacpi_config_spi_dev(spi, adev);
	if (ret)
		goto free_spi;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	acpi_set_modalias(adev, acpi_device_hid(adev), spi->modalias,
			  sizeof(spi->modalias));
#else
	strlcpy(spi->modalias, acpi_device_hid(adev), sizeof(spi->modalias));
#endif

	adev->power.flags.ignore_parent = true;

	ACPI_COMPANION_SET(&spi->dev, adev);
	acpi_device_set_enumerated(adev);

	/* add spi device */
	ret = spi_add_device(spi);
	if (ret) {
		adev->power.flags.ignore_parent = false;
		pr_err("Failed to add spi device: %d\n", ret);
		goto free_spi;
	}

	reg_info->spi = spi;

	pr_info("Added spi device %s\n", dev_name(&spi->dev));

	goto release_master;

free_spi:
	spi_dev_put(spi);
release_master:
	spi_master_put(spi_master);
	reg_info->spi_master = NULL;

	return ret;
}

static void appleacpi_dev_registration_worker(struct work_struct *work)
{
	struct appleacpi_spi_registration_info *info =
		container_of(work, struct appleacpi_spi_registration_info, work.work);

	if (info->spi_master && !info->spi_master->running) {
		pr_debug_ratelimited("spi-master device is not running yet\n");
		schedule_delayed_work(&info->work, usecs_to_jiffies(100));
		return;
	}

	appleacpi_register_spi_device(info->spi_master, info->adev);
}

/*
 * Callback for whenever a new master spi device is added.
 */
static int appleacpi_spi_master_added(struct device *dev,
				      struct class_interface *cif)
{
	struct spi_master *spi_master =
		container_of(dev, struct spi_master, dev);
	struct appleacpi_spi_registration_info *info =
		container_of(cif, struct appleacpi_spi_registration_info, cif);
	struct acpi_device *master_adev = spi_master->dev.parent ?
		ACPI_COMPANION(spi_master->dev.parent) : NULL;

	pr_debug("New spi-master device %s (%s) with bus-number %d was added\n",
		 dev_name(&spi_master->dev),
		 master_adev ? acpi_device_hid(master_adev) : "-no-acpi-dev-",
		 spi_master->bus_num);

	if (master_adev != info->adev->parent)
		return 0;

	pr_info("Got spi-master device for device %s\n",
		acpi_device_hid(info->adev));

	/*
	 * mutexes are held here, preventing unregistering of physical devices,
	 * so need to do the actual registration in a worker.
	 */
	info->spi_master = spi_master_get(spi_master);
	schedule_delayed_work(&info->work, usecs_to_jiffies(100));

	return 0;
}

/*
 * Callback for whenever a slave spi device is added or removed.
 */
static int appleacpi_spi_slave_changed(struct notifier_block *nb,
                                       unsigned long action, void *data)
{
	struct appleacpi_spi_registration_info *info =
		container_of(nb, struct appleacpi_spi_registration_info,
			     slave_notifier);
	struct spi_device *spi = data;

	pr_debug("SPI slave device changed: action=%lu, dev=%s\n",
		 action, dev_name(&spi->dev));

	switch (action) {
	case BUS_NOTIFY_DEL_DEVICE:
		if (spi == info->spi) {
			info->spi = NULL;
			return NOTIFY_OK;
		}
		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}

/*
 * spi_master_class is not exported, so this is an ugly hack to get it anyway.
 */
static struct class *appleacpi_get_spi_master_class(void)
{
	struct spi_master *spi_master;
	struct device dummy;
	struct class *cls = NULL;

	memset(&dummy, 0, sizeof(dummy));

	spi_master = spi_alloc_master(&dummy, 0);
	if (spi_master) {
		cls = spi_master->dev.class;
		spi_master_put(spi_master);
	}

	return cls;
}

static int appleacpi_probe(struct acpi_device *adev)
{
	struct appleacpi_spi_registration_info *reg_info;
	int ret;

	pr_debug("Probing acpi-device %s: bus-id='%s', adr=%lu, uid='%s'\n",
		 acpi_device_hid(adev), acpi_device_bid(adev),
		 acpi_device_adr(adev), acpi_device_uid(adev));

	ret = spi_register_driver(&applespi_driver);
	if (ret) {
		pr_err("Failed to register spi-driver: %d\n", ret);
		return ret;
	}

	/*
	 * Ideally we would just call spi_register_board_info() here,
	 * but that function is not exported. Additionally, we need to
	 * perform some extra work during device creation, such as
	 * unregistering physical devices. So instead we have do the
	 * registration ourselves. For that we see if our spi-master
	 * has been registered already, and if not jump through some
	 * hoops to make sure we are notified when it does.
	 */

	reg_info = kzalloc(sizeof(*reg_info), GFP_KERNEL);
	if (!reg_info) {
		pr_err("Failed to allocate registration-info\n");
		ret = -ENOMEM;
		goto unregister_driver;
	}

	reg_info->adev = adev;
	INIT_DELAYED_WORK(&reg_info->work, appleacpi_dev_registration_worker);

	adev->driver_data = reg_info;

	/*
	 * Set up listening for spi slave removals so we can properly
	 * handle them.
	 */
	reg_info->slave_notifier.notifier_call =
		appleacpi_spi_slave_changed;
	ret = bus_register_notifier(&spi_bus_type,
				    &reg_info->slave_notifier);
	if (ret) {
		pr_err("Failed to register notifier for spi slaves: %d\n", ret);
		goto free_reg_info;
	}

	/*
	 * Listen for additions of spi-master devices so we can register our spi
	 * device when the relevant master is added.  Note that our callback
	 * gets called immediately for all existing master devices, so this
	 * takes care of registration when the master already exists too.
	 */
	reg_info->cif.class = appleacpi_get_spi_master_class();
	reg_info->cif.add_dev = appleacpi_spi_master_added;

	ret = class_interface_register(&reg_info->cif);
	if (ret) {
		pr_err("Failed to register watcher for spi-master: %d\n", ret);
		goto unregister_notifier;
	}

	if (!reg_info->spi_master) {
		pr_info("No spi-master device found for device %s - waiting "
			"for it to be registered\n", acpi_device_hid(adev));
	}

	pr_info("acpi-device probe done: %s\n", acpi_device_hid(adev));

	return 0;

unregister_notifier:
	bus_unregister_notifier(&spi_bus_type, &reg_info->slave_notifier);
free_reg_info:
	adev->driver_data = NULL;
	kfree(reg_info);
unregister_driver:
	spi_unregister_driver(&applespi_driver);
	return ret;
}

static int appleacpi_remove(struct acpi_device *adev)
{
	struct appleacpi_spi_registration_info *reg_info;

	reg_info = acpi_driver_data(adev);
	if (reg_info) {
		class_interface_unregister(&reg_info->cif);
		bus_unregister_notifier(&spi_bus_type,
					&reg_info->slave_notifier);
		cancel_delayed_work_sync(&reg_info->work);
		if (reg_info->spi)
			spi_unregister_device(reg_info->spi);
		kfree(reg_info);
	}

	spi_unregister_driver(&applespi_driver);

	pr_info("acpi-device remove done: %s\n", acpi_device_hid(adev));

	return 0;
}

static struct acpi_driver appleacpi_driver = {
	.name		= "appleacpi",
	.class		= "topcase", /* ? */
	.owner		= THIS_MODULE,
	.ids		= ACPI_PTR(applespi_acpi_match),
	.ops		= {
		.add		= appleacpi_probe,
		.remove		= appleacpi_remove,
	},
};

module_acpi_driver(appleacpi_driver)

MODULE_LICENSE("GPL");
