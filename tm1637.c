#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/input/vivaldi-fmap.h>
#include <linux/serio.h>
#include <linux/workqueue.h>
#include <linux/libps2.h>
#include <linux/mutex.h>
#include <linux/dmi.h>
#include <linux/property.h>
#include <linux/kernel.h>

// local pin objects
static struct kbd *kbd;
static char pin_s[2]; // pin state
const char cDigit2Seg[] = {0x3f, 0x6, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};

#define CLOCK_DELAY 50

#define ATKBD_CMD_SETLEDS 0x10ed
#define ATKBD_CMD_GSCANSET 0x11f0
#define ATKBD_CMD_SSCANSET 0x10f0
#define ATKBD_CMD_GETID 0x02f2
#define ATKBD_CMD_SETREP 0x10f3
#define ATKBD_CMD_ENABLE 0x00f4
#define ATKBD_CMD_RESET_DIS 0x00f5	/* Reset to defaults and disable */
#define ATKBD_CMD_RESET_DEF 0x00f6	/* Reset to defaults */
#define ATKBD_CMD_SETALL_MB 0x00f8	/* Set all keys to give break codes */
#define ATKBD_CMD_SETALL_MBR 0x00fa /* ... and repeat */
#define ATKBD_CMD_RESET_BAT 0x02ff
#define ATKBD_CMD_RESEND 0x00fe
#define ATKBD_CMD_EX_ENABLE 0x10ea
#define ATKBD_CMD_EX_SETLEDS 0x20eb
#define ATKBD_CMD_OK_GETID 0x02e8

static const struct serio_device_id kbd_serio_ids[] = {
	{
		.type = SERIO_8042,
		.proto = SERIO_ANY,
		.id = SERIO_ANY,
		.extra = SERIO_ANY,
	},
	{
		.type = SERIO_8042_XL,
		.proto = SERIO_ANY,
		.id = SERIO_ANY,
		.extra = SERIO_ANY,
	},
	{
		.type = SERIO_RS232,
		.proto = SERIO_PS2SER,
		.id = SERIO_ANY,
		.extra = SERIO_ANY,
	},
	{0}};
struct kbd
{

	struct ps2dev ps2dev;
	struct input_dev *dev;

	/* Written only during init */
	char name[64];
	char phys[32];

	unsigned short id;
	unsigned short keycode[512];
	DECLARE_BITMAP(force_release_mask, 512);
	unsigned char set;
	bool translated;
	bool extra;
	bool write;
	bool softrepeat;
	bool softraw;
	bool scroll;
	bool enabled;

	/* Accessed only from interrupt */
	unsigned char emul;
	bool resend;
	bool release;
	unsigned long xl_bit;
	unsigned int last;
	unsigned long time;
	unsigned long err_count;

	struct delayed_work event_work;
	unsigned long event_jiffies;
	unsigned long event_mask;

	/* Serializes reconnect(), attr->set() and event work */
	struct mutex mutex;

	struct vivaldi_data vdata;
};
static irqreturn_t kbd_interrupt(struct serio *serio, unsigned char data,
								 unsigned int flags)
{
	struct kbd *kbd = serio_get_drvdata(serio);
	if (unlikely(kbd->ps2dev.flags & PS2_FLAG_ACK))
		if (ps2_handle_ack(&kbd->ps2dev, data))
			goto out;

	if (unlikely(kbd->ps2dev.flags & PS2_FLAG_CMD))
		if (ps2_handle_response(&kbd->ps2dev, data))
			goto out;
out:
	return IRQ_HANDLED;
}


static void pin_on(int pin)
{
	// 0: num lock
	// 1: caps lock
	if (pin)
	{
		pin_s[0] &= ~4;
	}
	else
	{
		pin_s[0] &= ~2;
	}

	ps2_command(&kbd->ps2dev, pin_s, ATKBD_CMD_SETLEDS);
}
static void pin_off(int pin)
{
	// 0: num lock
	// 1: caps lock
	if (pin)
	{
		pin_s[0] |= 4;
	}
	else
	{
		pin_s[0] |= 2;
	}

	ps2_command(&kbd->ps2dev, pin_s, ATKBD_CMD_SETLEDS);
}
static void tm1637_init(void)
{
	pin_off(0);
	pin_off(1);
}
static void tm1637_start(void)
{
	pin_on(1);
	pin_on(0);
	mdelay(CLOCK_DELAY);
	pin_off(1);
}
static void tm1637_stop(void)
{
	pin_off(0);
	mdelay(CLOCK_DELAY);
	pin_off(1);
	mdelay(CLOCK_DELAY);
	pin_on(0);
	mdelay(CLOCK_DELAY);
	pin_on(1);
}
static void tm1637_ack(void)
{
	pin_off(0);
	pin_on(1);
	mdelay(CLOCK_DELAY);
	pin_on(0);
	mdelay(CLOCK_DELAY);
	pin_off(0);
}
static void tm1637_write_byte(uint8_t b)
{
	for (int i = 0; i < 8; i++)
	{
		pin_off(0);
		if (b & 1)
		{
			pin_on(1);
		}
		else
		{
			pin_off(1);
		}
		mdelay(CLOCK_DELAY);
		pin_on(0);
		mdelay(CLOCK_DELAY);
		b >>= 1;
	}
}
static void tm1637_write(uint8_t *pData, uint8_t bLen)
{
	tm1637_start();
	for (int b = 0; b < bLen; b++)
	{
		tm1637_write_byte(pData[b]);
		tm1637_ack();
	}
	tm1637_stop();
}
static void tm1637_set_brightness(int brightness)
{
	uint8_t brightness_control;
	if (brightness == 0)
	{
		brightness_control = 0x80;
	}
	else
	{
		if (brightness > 8)
		{
			brightness = 8;
		}
		brightness_control = 0x88 | (brightness - 1);
	}
	tm1637_write(&brightness_control, 1);
}
void tm1637ShowDigits(char *pString)
{
	char b, bTemp[16]; // commands and data to transmit
	int i, j;

	j = 0;
	bTemp[0] = 0x40; // memory write command (auto increment mode)
	tm1637_write(bTemp, 1);

	bTemp[j++] = 0xc0; // set display address to first digit command
	for (i = 0; i < 5; i++)
	{
		if (i == 2) // position of the colon
		{
			if (pString[i] == ':') // turn on correct bit
				bTemp[2] |= 0x80;  // second digit high bit controls colon LEDs
		}
		else
		{
			b = 0;
			if (pString[i] >= '0' && pString[i] <= '9')
			{
				b = cDigit2Seg[pString[i] & 0xf]; // segment data
			}
			bTemp[j++] = b;
		}
	}
	tm1637_write(bTemp, j); // send to the display
}

static int kbd_connect(struct serio *serio, struct serio_driver *drv)
{
	if (serio->id.type == 6)
	{
		kbd = kzalloc(sizeof(struct kbd), GFP_KERNEL);
		ps2_init(&kbd->ps2dev, serio);

		serio_set_drvdata(serio, kbd);
		serio_open(serio, drv);
		tm1637_init();
		tm1637_set_brightness(8);
		tm1637ShowDigits("01:23");
		serio_close(serio);
		serio_set_drvdata(serio, NULL);
		kfree(kbd);
	}
	return 0;
}
static void kbd_disconnect(struct serio *serio)
{
	serio_close(serio);
	serio_set_drvdata(serio, NULL);
}
MODULE_DEVICE_TABLE(serio, kbd_serio_ids);
static struct serio_driver kbd_drv = {
	.driver = {
		.name = "kbd",
	},
	.description = "TM1637 communication through serio",
	.id_table = kbd_serio_ids,
	.interrupt = kbd_interrupt,
	.connect = kbd_connect,
	.reconnect = NULL,
	.disconnect = kbd_disconnect,
	.cleanup = NULL,
};
int init_module(void)
{
	serio_register_driver(&kbd_drv);
	return 0;
}

void cleanup_module(void)
{
	serio_unregister_driver(&kbd_drv);
}

MODULE_LICENSE("GPL");

