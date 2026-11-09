// SPDX-License-Identifier: GPL-2.0-only
/*
 * almond_lcd - минимальный драйвер экрана Securifi Almond 3S, только вывод.
 *
 * Панель CDTech S028HQ29NN, контроллер ILI9341, 320x240, RGB565. Подключена
 * 8-битной шиной 8080, которую мы дёргаем GPIO-линиями MT7621 (банк 0).
 * Распиновка и таблица инициализации панели взяты из GPL-драйвера
 * fildunsky/openwrt-almond3s (по разбору iSublimity/Securifi-Almond-3S).
 *
 * Чем отличается от того драйвера:
 *  - только экран: нет I2C, тача, PIC, программного ШИМ подсветки;
 *  - данные на шину идут атомарными DSET/DCLR, регистр DATA целиком не
 *    перезаписывается, поэтому чужие линии банка 0 (PERST PCIe на GPIO19,
 *    подсветка на GPIO31) драйвер не трогает;
 *  - группы выводов jtag (GPIO13-17) и wdt (GPIO18) переводятся в GPIO
 *    точечной правкой битов 7 и 8-9 регистра GPIOMODE: в DTS прошивки
 *    24.10.5 для Almond они не заявлены.
 *
 * Подсветка (GPIO31) остаётся за штатным светодиодом display:power:
 *   echo 1 > /sys/class/leds/display:power/brightness
 *
 * Интерфейс: /dev/lcd - кадр 320*240 пикселей u16 RGB565 в порядке байт
 * процессора, строка за строкой (640 байт на строку). write()/pwrite()
 * принимают только целые строки: смещение и длина кратны 640. Записанные
 * строки сразу уходят на панель.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/minmax.h>

#define PALMBUS_BASE	0x1E000000
#define PALMBUS_SIZE	0x1000
#define REG_GPIOMODE	0x060
#define REG_CTRL0	0x600	/* направление банка 0, 1 = выход */
#define REG_DSET0	0x630	/* запись 1 в бит поднимает линию */
#define REG_DCLR0	0x640	/* запись 1 в бит опускает линию */

#define GPIOMODE_JTAG_GPIO	BIT(7)		/* группа jtag: 1 = GPIO */
#define GPIOMODE_WDT_MASK	(3u << 8)	/* группа wdt: 1 = GPIO */
#define GPIOMODE_WDT_GPIO	(1u << 8)
#define GPIOMODE_RGMII2_GPIO	BIT(15)		/* группа rgmii2 (D2-D7): 1 = GPIO, ставит DTS */

/* Линии банка 0: номер бита = номер GPIO. */
#define B_D0	BIT(13)
#define B_D1	BIT(18)
#define B_D2	BIT(22)
#define B_D3	BIT(23)
#define B_D4	BIT(24)
#define B_D5	BIT(25)
#define B_D6	BIT(26)
#define B_D7	BIT(27)
#define B_DC	BIT(14)	/* 0 = команда, 1 = данные (у сообщества названа WRX) */
#define B_RST	BIT(15)	/* сброс панели, активен низким */
#define B_CS	BIT(16)	/* выбор чипа, активен низким */
#define B_WR	BIT(17)	/* строб записи, защёлка по фронту (у сообщества DCX) */

#define DATA_MASK (B_D0 | B_D1 | B_D2 | B_D3 | B_D4 | B_D5 | B_D6 | B_D7)
#define LCD_MASK  (DATA_MASK | B_DC | B_RST | B_CS | B_WR)

#define LCD_W		320
#define LCD_H		240
#define ROW_BYTES	(LCD_W * 2)
#define FB_BYTES	(ROW_BYTES * LCD_H)
#define CHUNK_ROWS	16

static int rotate;
module_param(rotate, int, 0444);
MODULE_PARM_DESC(rotate, "1 = повернуть изображение на 180 градусов");

static void __iomem *pb;
static u32 set_tbl[256];
static u16 *chunk;
static DEFINE_MUTEX(lcd_lock);

static inline void pw(u32 off, u32 v) { __raw_writel(v, pb + off); }
static inline u32 pr(u32 off) { return __raw_readl(pb + off); }

static void build_tbl(void)
{
	static const u32 bits[8] = { B_D0, B_D1, B_D2, B_D3, B_D4, B_D5, B_D6, B_D7 };
	int v, b;

	for (v = 0; v < 256; v++) {
		u32 s = 0;

		for (b = 0; b < 8; b++)
			if (v & (1 << b))
				s |= bits[b];
		set_tbl[v] = s;
	}
}

/* Один байт на шину: нули данных и строб вниз, единицы данных, фронт строба. */
static inline void bus_byte(u8 v)
{
	u32 s = set_tbl[v];

	pw(REG_DCLR0, (DATA_MASK & ~s) | B_WR);
	pw(REG_DSET0, s);
	pw(REG_DSET0, B_WR);
}

static void lcd_cmd(u8 c)
{
	pw(REG_DCLR0, B_DC);
	bus_byte(c);
	pw(REG_DSET0, B_DC);
}

static void lcd_cmd_data(u8 c, const u8 *d, int n)
{
	int i;

	lcd_cmd(c);
	for (i = 0; i < n; i++)
		bus_byte(d[i]);
}

/* Таблица загрузчика (как в драйвере сообщества): команда, число байт, байты. */
static const u8 ili9341_init[] = {
	0xCF, 3, 0x00, 0xC1, 0x30,
	0xED, 4, 0x64, 0x03, 0x12, 0x81,
	0xE8, 3, 0x85, 0x00, 0x78,
	0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
	0xF7, 1, 0x20,
	0xEA, 2, 0x00, 0x00,
	0xC0, 1, 0x1B,
	0xC1, 1, 0x11,
	0xC5, 2, 0x3F, 0x3C,
	0xC7, 1, 0x8E,
	0x3A, 1, 0x55,			/* 16 бит на пиксель */
	0xB1, 2, 0x00, 0x15,
	0xB6, 2, 0x0A, 0xA2,
	0xF2, 1, 0x00,
	0x26, 1, 0x01,
	0xE0, 15, 0x0F, 0x0C, 0x0B, 0x07, 0x09, 0x00, 0x41, 0x67,
		  0x37, 0x07, 0x12, 0x06, 0x0F, 0x09, 0x00,
	0xE1, 15, 0x00, 0x0B, 0x0E, 0x03, 0x0F, 0x04, 0x2C, 0x16,
		  0x43, 0x02, 0x0B, 0x0A, 0x2F, 0x30, 0x0F,
};

static void lcd_window(int r0, int r1)
{
	u8 col[4] = { 0, 0, (LCD_W - 1) >> 8, (LCD_W - 1) & 0xFF };
	u8 row[4] = { r0 >> 8, r0 & 0xFF, r1 >> 8, r1 & 0xFF };

	lcd_cmd_data(0x2A, col, 4);
	lcd_cmd_data(0x2B, row, 4);
	lcd_cmd(0x2C);
}

static void lcd_pixels(const u16 *p, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		bus_byte(p[i] >> 8);
		bus_byte(p[i] & 0xFF);
		/* ядро без вытеснения: отдаём процессор каждые 256 пикселей (~0.1 мс) */
		if ((i & 255) == 255)
			cond_resched();
	}
}

static void lcd_fill(u16 color)
{
	int r, i;

	for (i = 0; i < CHUNK_ROWS * LCD_W; i++)
		chunk[i] = color;
	for (r = 0; r < LCD_H; r += CHUNK_ROWS) {
		lcd_window(r, r + CHUNK_ROWS - 1);
		lcd_pixels(chunk, CHUNK_ROWS * LCD_W);
		cond_resched();
	}
}

static void lcd_panel_init(void)
{
	const u8 *p = ili9341_init;
	u8 madctl = rotate ? 0x68 : 0xA8;	/* альбомная, порядок BGR */

	/* аппаратный сброс */
	pw(REG_DSET0, B_RST);
	msleep(10);
	pw(REG_DCLR0, B_RST);
	msleep(20);
	pw(REG_DSET0, B_RST);
	msleep(120);

	pw(REG_DCLR0, B_CS);
	while (p < ili9341_init + sizeof(ili9341_init)) {
		lcd_cmd_data(p[0], p + 2, p[1]);
		p += 2 + p[1];
	}
	lcd_cmd_data(0x36, &madctl, 1);
	lcd_cmd(0x11);			/* выход из сна */
	msleep(120);
	lcd_fill(0x0000);
	lcd_cmd(0x29);			/* дисплей включён */
	pw(REG_DSET0, B_CS);
}

static ssize_t lcd_write(struct file *f, const char __user *buf, size_t len, loff_t *ppos)
{
	loff_t pos = *ppos;
	size_t done = 0;
	ssize_t err = 0;

	if (pos < 0 || pos % ROW_BYTES || len % ROW_BYTES)
		return -EINVAL;
	if (pos >= FB_BYTES)
		return len ? -ENOSPC : 0;
	len = min_t(size_t, len, FB_BYTES - pos);

	/* gpio-mmio держит копию CTRL0 со времени загрузки: смена направления
	 * любой линии банка 0 через gpiolib вернёт наши линии во входы. */
	if ((pr(REG_CTRL0) & LCD_MASK) != LCD_MASK) {
		pr_warn_ratelimited("almond_lcd: LCD pins are no longer outputs (CTRL0 0x%08x), reload the module\n",
				    pr(REG_CTRL0));
		return -EIO;
	}
	if (mutex_lock_interruptible(&lcd_lock))
		return -ERESTARTSYS;
	pw(REG_DCLR0, B_CS);
	while (done < len) {
		size_t n = min_t(size_t, len - done, CHUNK_ROWS * ROW_BYTES);
		int row = (pos + done) / ROW_BYTES;

		if (copy_from_user(chunk, buf + done, n)) {
			err = -EFAULT;
			break;
		}
		lcd_window(row, row + n / ROW_BYTES - 1);
		lcd_pixels(chunk, n / 2);
		done += n;
		cond_resched();
	}
	pw(REG_DSET0, B_CS);
	mutex_unlock(&lcd_lock);

	if (!done)
		return err;
	*ppos = pos + done;
	return done;
}

static loff_t lcd_llseek(struct file *f, loff_t off, int whence)
{
	return fixed_size_llseek(f, off, whence, FB_BYTES);
}

static const struct file_operations lcd_fops = {
	.owner	= THIS_MODULE,
	.write	= lcd_write,
	.llseek	= lcd_llseek,
};

static struct miscdevice lcd_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "lcd",
	.fops	= &lcd_fops,
	.mode	= 0600,
};

static int __init almond_lcd_init(void)
{
	u32 mode, nmode, ctrl, nctrl;
	int ret;

	if (!of_machine_is_compatible("securifi,almond-3s")) {
		pr_err("almond_lcd: not a Securifi Almond 3S, refusing to load\n");
		return -ENODEV;
	}

	chunk = kmalloc(CHUNK_ROWS * ROW_BYTES, GFP_KERNEL);
	if (!chunk)
		return -ENOMEM;
	pb = ioremap(PALMBUS_BASE, PALMBUS_SIZE);
	if (!pb) {
		kfree(chunk);
		return -ENOMEM;
	}
	build_tbl();

	mode = pr(REG_GPIOMODE);
	if (!(mode & GPIOMODE_RGMII2_GPIO)) {
		pr_err("almond_lcd: rgmii2 pins are not GPIO (GPIOMODE 0x%08x), refusing to load\n", mode);
		iounmap(pb);
		kfree(chunk);
		return -ENODEV;
	}
	nmode = ((mode | GPIOMODE_JTAG_GPIO) & ~GPIOMODE_WDT_MASK) | GPIOMODE_WDT_GPIO;
	if (nmode != mode)
		pw(REG_GPIOMODE, nmode);

	/* сначала уровни покоя (CS, WR, DC, RST высокие), потом линии на выход */
	pw(REG_DSET0, B_CS | B_WR | B_DC | B_RST);
	ctrl = pr(REG_CTRL0);
	nctrl = ctrl | LCD_MASK;
	pw(REG_CTRL0, nctrl);
	pr_info("almond_lcd: GPIOMODE 0x%08x -> 0x%08x, CTRL0 0x%08x -> 0x%08x\n",
		mode, nmode, ctrl, nctrl);

	lcd_panel_init();

	ret = misc_register(&lcd_misc);
	if (ret) {
		pw(REG_DCLR0, B_CS);
		lcd_cmd(0x28);
		lcd_cmd(0x10);
		pw(REG_DSET0, B_CS);
		iounmap(pb);
		kfree(chunk);
		return ret;
	}
	pr_info("almond_lcd: ILI9341 320x240 ready on /dev/lcd\n");
	return 0;
}

static void __exit almond_lcd_exit(void)
{
	misc_deregister(&lcd_misc);
	mutex_lock(&lcd_lock);
	pw(REG_DCLR0, B_CS);
	lcd_cmd(0x28);			/* дисплей выключен */
	lcd_cmd(0x10);			/* сон */
	pw(REG_DSET0, B_CS);
	mutex_unlock(&lcd_lock);
	iounmap(pb);
	kfree(chunk);
}

module_init(almond_lcd_init);
module_exit(almond_lcd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Display-only driver for the Securifi Almond 3S ILI9341 panel");
