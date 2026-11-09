// SPDX-License-Identifier: GPL-2.0-only
/*
 * almond-lcd-status - экран состояния Almond 3S поверх /dev/lcd (almond_lcd.ko).
 *
 * Что на экране:
 *  - шапка: общий статус, по центру часы с мигающей точкой (замер экрана =
 *    роутер завис), справа батарейка на 5 делений;
 *  - CPU и RAM в процентах с полосой; под CPU полоски отдельных ядер, под RAM МБ;
 *  - WAN: приём/отдача и график за 2 минуты;
 *  - Wi-Fi 2.4 и 5 ГГц: стрелки приёма/отдачи моргают при трафике, устройства;
 *  - podkop, sing-box, ping, туннель агента, аптайм;
 *  - мобильная сеть (внешний модем): статус словом, стрелки WAN, диапазоны.
 *
 * Текст сглаженным шрифтом JetBrains Mono (font_aa.h, SIL OFL 1.1) в тех же
 * ячейках 8x16, что у прежнего растрового, раскладка экрана прежняя.
 *
 * Вторая страница - внешний модем: процессор, память, сигнал с графиком
 * SINR за час, агрегация, сота, подключение (вывод almond-lcd-m2 с модема).
 *
 * Тач SX8650 на I2C 0x48 (опрос через /dev/i2c-0): тап (или свайп) листает
 * страницы, удержание ~0,8 с переключает тёмную и светлую тему; со
 * страницы модема через 5 минут без касаний - обратно на главную. Тема
 * хранится в /etc/almond-lcd.theme; переключить без рук: kill -USR1 <pid>.
 * С PIC на 0x2A той же шины раз в 15 с читается заряд батареи (заводские
 * команды 0x39 и 0x36, только чтение).
 *
 * Быстрые метрики читаются из /proc, /sys и debugfs; медленные раз в минуту
 * пишет almond-lcd-collect в /tmp/almond-lcd.status. Тач опрашивается каждые
 * 50 мс, экран перерисовывается раз в секунду (и сразу после смены темы),
 * на панель уходят только изменившиеся строки.
 *
 * Запуск: almond-lcd-status [wan] [wifi-2.4] [wifi-5]
 *         (по умолчанию wan phy1-ap0 phy0-ap0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <syslog.h>
#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#endif
#include "font_aa.h"

#define W 320
#define H 240
#define HIST 120
#define THEME_FILE "/etc/almond-lcd.theme"

/* знаки вне ASCII в font_aa.h */
#define S_DN	"\x80"
#define S_UP	"\x81"
#define S_DEG	"\x82"

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

struct theme {
	const char *name;
	uint16_t bg, panel, track, text, dim, idle, ok, lit, warn, bad, rx, tx, grid;
};

static const struct theme themes[2] = {
	{ "dark",
	  RGB(10, 12, 18), RGB(26, 30, 40), RGB(48, 54, 68), RGB(235, 238, 245),
	  RGB(140, 148, 165), RGB(70, 76, 92), RGB(60, 200, 110), RGB(90, 255, 140),
	  RGB(245, 190, 40), RGB(235, 70, 60), RGB(70, 190, 245), RGB(245, 150, 60),
	  RGB(38, 43, 56) },
	{ "light",
	  RGB(236, 239, 244), RGB(212, 217, 226), RGB(188, 194, 206), RGB(18, 22, 30),
	  RGB(88, 96, 112), RGB(165, 171, 184), RGB(20, 150, 70), RGB(0, 200, 80),
	  RGB(205, 135, 0), RGB(205, 40, 30), RGB(0, 115, 200), RGB(215, 100, 0),
	  RGB(198, 203, 214) },
};
static int theme_idx;

#define TH	 (&themes[theme_idx])
#define C_BG	 (TH->bg)
#define C_PANEL	 (TH->panel)
#define C_TRACK	 (TH->track)
#define C_TEXT	 (TH->text)
#define C_DIM	 (TH->dim)
#define C_IDLE	 (TH->idle)
#define C_OK	 (TH->ok)
#define C_LIT	 (TH->lit)
#define C_WARN	 (TH->warn)
#define C_BAD	 (TH->bad)
#define C_RX	 (TH->rx)
#define C_TX	 (TH->tx)
#define C_GRID	 (TH->grid)

static uint16_t fb[H][W], shown[H][W];
static int shown_valid;
static volatile sig_atomic_t stop, toggle_req;

static void on_signal(int s) { (void)s; stop = 1; }
static void on_usr1(int s) { (void)s; toggle_req = 1; }

/* ---------- рисование ---------- */

/* Смесь двух цветов RGB565: k = 0 - a, k = 15 - b. */
static uint16_t mix(uint16_t a, uint16_t b, int k)
{
	int r = ((a >> 11) * (15 - k) + (b >> 11) * k + 7) / 15;
	int g = (((a >> 5) & 63) * (15 - k) + ((b >> 5) & 63) * k + 7) / 15;
	int bl = ((a & 31) * (15 - k) + (b & 31) * k + 7) / 15;

	return (uint16_t)(r << 11 | g << 5 | bl);
}

static void fill(int x, int y, int w, int h, uint16_t c)
{
	int i, j;

	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > W) w = W - x;
	if (y + h > H) h = H - y;
	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			fb[j][i] = c;
}

/* Строка сглаженным шрифтом, края букв смешиваются с тем, что уже нарисовано.
 * (x, y) - угол ячейки прежнего шрифта 8x16; sc = 1 - обычный текст, 2 - крупные
 * цифры. Ширина знака та же (8 и 16), базовая линия на прежнем месте (строка 12
 * ячейки, у крупных 24), высота заглавных та же. Возвращает x после строки. */
static int text(int x, int y, const char *s, uint16_t c, int sc)
{
	const struct aafont *f = sc > 1 ? &font_xxl : &font_mm;

	y += 12 * sc - f->base;
	for (; *s; s++, x += f->adv) {
		unsigned ch = (unsigned char)*s;
		const uint8_t *g;
		int i, j;

		if (ch < f->first || ch > f->last)
			continue;
		g = f->data + f->off[ch - f->first];
		for (j = 0; j < f->h; j++)
			for (i = 0; i < f->sw; i++) {
				int n = j * f->sw + i, a = n & 1 ? g[n >> 1] & 15 : g[n >> 1] >> 4;
				int px = x - 1 + i, py = y + j;

				if (a && px >= 0 && px < W && py >= 0 && py < H)
					fb[py][px] = a == 15 ? c : mix(fb[py][px], c, a);
			}
	}
	return x;
}

static int text_w(const char *s, int sc) { return (int)strlen(s) * 8 * sc; }

static void text_r(int xr, int y, const char *s, uint16_t c, int sc)
{
	text(xr - text_w(s, sc), y, s, c, sc);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int e = dx + dy;

	for (;;) {
		if (x0 >= 0 && x0 < W && y0 >= 0 && y0 < H)
			fb[y0][x0] = c;
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * e;
		if (e2 >= dy) { e += dy; x0 += sx; }
		if (e2 <= dx) { e += dx; y0 += sy; }
	}
}

static uint16_t level_color(int pct, int warn, int bad)
{
	return pct < warn ? C_OK : pct < bad ? C_WARN : C_BAD;
}

static void bar(int x, int y, int w, int h, int pct, uint16_t c)
{
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	fill(x, y, w, h, C_TRACK);
	fill(x, y, w * pct / 100, h, c);
}

static void dot(int cx, int cy, int r, uint16_t c)
{
	int x, y;

	for (y = -r; y <= r; y++)
		for (x = -r; x <= r; x++)
			if (x * x + y * y <= r * r + r)
				fill(cx + x, cy + y, 1, 1, c);
}

/* Значок Wi-Fi: три дуги и точка; (cx, cy) - нижняя точка, размер ~22x16. */
static void wifi_icon(int cx, int cy, uint16_t c)
{
	static const float rad[3] = { 4.5f, 9.0f, 13.5f };
	int x, y, k;

	for (y = -15; y <= 0; y++)
		for (x = -15; x <= 15; x++) {
			float d = sqrtf((float)(x * x + y * y));

			if (y > -1 || abs(x) > -y)	/* сектор 45..135 градусов */
				continue;
			for (k = 0; k < 3; k++)
				if (fabsf(d - rad[k]) < 1.1f)
					fill(cx + x, cy + y, 1, 1, c);
		}
	dot(cx, cy - 1, 1, c);
}

/* ---------- метрики ---------- */

static int read_file(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -1;
	buf[r] = 0;
	return (int)r;
}

#define MAXCPU 8

/* Строка /proc/stat "cpu ..." или "cpuN ...": счётчики всего и простоя.
 * Возвращает индекс: 0 - общий, N+1 - поток N, -1 - не та строка. */
static int cpu_line(const char *ln, unsigned long long *tot, unsigned long long *idle)
{
	unsigned long long v[8] = { 0 };
	const char *sp = strchr(ln, ' ');
	int idx, id;

	if (strncmp(ln, "cpu", 3) || !sp)
		return -1;
	if (ln[3] == ' ')
		idx = 0;
	else if (sscanf(ln + 3, "%d", &id) == 1 && id >= 0 && id < MAXCPU)
		idx = id + 1;
	else
		return -1;
	if (sscanf(sp, "%llu %llu %llu %llu %llu %llu %llu %llu",
		   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4)
		return -1;
	*idle = v[3] + v[4];
	*tot = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
	return idx;
}

/* Счётчики /proc/stat: [0] - общий, [1..n] - отдельные ядра. Возвращает n. */
static int cpu_sample(unsigned long long tot[MAXCPU + 1], unsigned long long idle[MAXCPU + 1])
{
	char b[2048], *ln, *save;
	int n = 0;

	if (read_file("/proc/stat", b, sizeof(b)) < 0)
		return -1;
	for (ln = strtok_r(b, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
		unsigned long long t, i;
		int idx;

		if (strncmp(ln, "cpu", 3))
			break;
		if ((idx = cpu_line(ln, &t, &i)) < 0)
			continue;
		tot[idx] = t;
		idle[idx] = i;
		if (idx > n)
			n = idx;
	}
	return n;
}

static void mem_sample(long *total_kb, long *avail_kb)
{
	char b[2048], *p;

	*total_kb = *avail_kb = 0;
	if (read_file("/proc/meminfo", b, sizeof(b)) < 0)
		return;
	if ((p = strstr(b, "MemTotal:")))
		*total_kb = strtol(p + 9, NULL, 10);
	if ((p = strstr(b, "MemAvailable:")))
		*avail_kb = strtol(p + 13, NULL, 10);
}

static unsigned long long read_ull(const char *path)
{
	char b[64];

	return read_file(path, b, sizeof(b)) < 0 ? 0 : strtoull(b, NULL, 10);
}

static unsigned long long net_stat(const char *ifname, const char *what)
{
	char p[160];

	snprintf(p, sizeof(p), "/sys/class/net/%s/statistics/%s", ifname, what);
	return read_ull(p);
}

/* Процесс жив? Сравниваем comm и, если задано, ищем подстроку в cmdline. */
static int proc_alive(const char *comm, const char *cmd_sub)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	char path[300], b[512];
	int found = 0;

	if (!d)
		return 0;
	while (!found && (e = readdir(d))) {
		int n, i;

		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
		if (read_file(path, b, sizeof(b)) < 0)
			continue;
		b[strcspn(b, "\n")] = 0;
		if (strcmp(b, comm))
			continue;
		if (!cmd_sub) {
			found = 1;
			break;
		}
		snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
		n = read_file(path, b, sizeof(b));
		for (i = 0; i < n; i++)
			if (!b[i])
				b[i] = ' ';
		if (n > 0 && strstr(b, cmd_sub))
			found = 1;
	}
	closedir(d);
	return found;
}

/* ---------- Wi-Fi и WAN: активность по счётчикам пакетов ---------- */

struct wifi {
	const char *ifname;
	const char *label;
	int up;				/* operstate up */
	int clients;			/* станций по debugfs, -1 = неизвестно */
	int rx_act, tx_act;		/* был трафик за последнюю секунду */
	unsigned long long rx_pk, tx_pk;
};

static void wifi_sample(struct wifi *w, int full)
{
	char p[200], b[32];
	unsigned long long rx = net_stat(w->ifname, "rx_packets");
	unsigned long long tx = net_stat(w->ifname, "tx_packets");

	w->rx_act = w->rx_pk && rx > w->rx_pk;
	w->tx_act = w->tx_pk && tx > w->tx_pk;
	w->rx_pk = rx;
	w->tx_pk = tx;
	if (!full)
		return;

	snprintf(p, sizeof(p), "/sys/class/net/%s/operstate", w->ifname);
	w->up = read_file(p, b, sizeof(b)) > 0 && !strncmp(b, "up", 2);

	w->clients = -1;
	snprintf(p, sizeof(p), "/sys/class/net/%s/phy80211/name", w->ifname);
	if (read_file(p, b, sizeof(b)) > 0) {
		char sp[200];
		DIR *d;
		struct dirent *e;

		b[strcspn(b, "\n")] = 0;
		snprintf(sp, sizeof(sp), "/sys/kernel/debug/ieee80211/%s/netdev:%s/stations", b, w->ifname);
		if ((d = opendir(sp))) {
			w->clients = 0;
			while ((e = readdir(d)))
				if (e->d_name[0] != '.')
					w->clients++;
			closedir(d);
		}
	}
}

/* ---------- тач SX8650 ---------- */

/* Калибровка как у сообщества для Almond 3S при той же ориентации панели
 * (MADCTL 0xA8): сырое X - вертикаль (верх, низ), сырое Y - горизонталь
 * (лево, право). Своя - в /etc/almond-lcd.tcal ("216 3911 3952 120"). */
#define TCAL_FILE "/etc/almond-lcd.tcal"
static int tcal[4] = { 216, 3911, 3952, 120 };
static int raw_x = -1, raw_y = -1;	/* последние сырые координаты касания */
static int touch_x = -1, touch_y = -1;	/* они же в пикселях экрана */

static void tcal_load(void)
{
	char b[64];
	int v[4];

	if (read_file(TCAL_FILE, b, sizeof(b)) > 0 &&
	    sscanf(b, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) == 4 && v[0] != v[1] && v[2] != v[3])
		memcpy(tcal, v, sizeof(tcal));
}

static void touch_map(void)
{
	int x, y;

	if (raw_x < 0 || raw_y < 0)
		return;
	x = (raw_y - tcal[2]) * (W - 1) / (tcal[3] - tcal[2]);
	y = (raw_x - tcal[0]) * (H - 1) / (tcal[1] - tcal[0]);
	touch_x = x < 0 ? 0 : x > W - 1 ? W - 1 : x;
	touch_y = y < 0 ? 0 : y > H - 1 ? H - 1 : y;
}

static void touch_reset(void)
{
	raw_x = raw_y = touch_x = touch_y = -1;
}

#ifdef __linux__
#define SX_ADDR 0x48
static int tfd = -1;

static int sx_xfer(uint8_t *buf, int len, int rd)
{
	struct i2c_msg m = { .addr = SX_ADDR, .flags = rd ? I2C_M_RD : 0, .len = len, .buf = buf };
	struct i2c_rdwr_ioctl_data d = { .msgs = &m, .nmsgs = 1 };

	return ioctl(tfd, I2C_RDWR, &d) == 1 ? 0 : -1;
}

static int sx_wr2(uint8_t a, uint8_t b)
{
	uint8_t x[2] = { a, b };

	return sx_xfer(x, 2, 0);
}

/* Настройка как в рабочем режиме драйвера сообщества (PENTRG через Linux I2C):
 * сброс, 5000 выборок/с, усреднение 7, прерывание только при касании, каналы X|Y,
 * непрерывный режим ожидания пера. */
static void touch_init(void)
{
	static const uint8_t cfg[5][2] = {
		{ 0x00, 0x00 }, { 0x01, 0xF7 }, { 0x02, 0x23 }, { 0x03, 0x2D }, { 0x04, 0xC0 },
	};
	uint8_t c, v[6];
	int i;

	if (tfd < 0)
		tfd = open("/dev/i2c-0", O_RDWR);
	if (tfd < 0) {
		syslog(LOG_WARNING, "touch: /dev/i2c-0: %s", strerror(errno));
		return;
	}
	if (sx_wr2(0x1F, 0xDE) < 0) {
		syslog(LOG_WARNING, "touch: SX8650 does not answer at 0x48");
		close(tfd);
		tfd = -1;
		return;
	}
	usleep(50000);
	for (i = 0; i < 5; i++)
		sx_wr2(cfg[i][0], cfg[i][1]);
	c = 0xE0;
	sx_xfer(&c, 1, 0);
	for (i = 0; i < 6; i++) {
		c = 0x40 | i;
		v[i] = 0xEE;
		if (!sx_xfer(&c, 1, 0))
			sx_xfer(&v[i], 1, 1);
	}
	syslog(LOG_INFO, "touch: SX8650 ready, regs %02x %02x %02x %02x %02x %02x",
	       v[0], v[1], v[2], v[3], v[4], v[5]);
}

/* 1 = за этот опрос пришли отсчёты пера, 0 = пера нет, -1 = ошибка шины.
 * Отсчёт - [0|канал(3)|данные(12)], канал 0 = X, 1 = Y. */
static int touch_poll(void)
{
	uint8_t c = 0x45, st, d[2];
	int n, got = 0;

	if (tfd < 0)
		return -1;
	for (n = 0; n < 8; n++) {
		uint16_t v;

		if (sx_xfer(&c, 1, 0) || sx_xfer(&st, 1, 1))
			return -1;
		if (!(st & 0x80))		/* CONVIRQ: есть свежая конверсия */
			break;
		if (sx_xfer(d, 2, 1))
			return -1;
		v = (uint16_t)(d[0] << 8 | d[1]);
		if (v == 0xFFFF || (v & 0x8000))
			return 0;
		if ((v >> 12) == 0)
			raw_x = v & 0xFFF;
		else if ((v >> 12) == 1)
			raw_y = v & 0xFFF;
		got = 1;
	}
	if (got)
		touch_map();
	return got;
}

#else	/* предпросмотр на хосте: тача нет */
static int tfd = -1;
static void touch_init(void) { }
static int touch_poll(void) { return -1; }
#endif

/* Жесты по опросам каждые 50 мс. Касание засчитывается после двух опросов с
 * пером подряд, отпускание - после 0,2 с без пера. Долгое нажатие - 0,8 с
 * почти на месте, срабатывает не дожидаясь отпускания. Свайп - сдвиг по
 * горизонтали от 60 пикселей и больше, чем по вертикали. Без координат любое
 * касание - просто тап. */
enum { G_NONE, G_TAP, G_TAP_L, G_TAP_M, G_TAP_R, G_SWIPE_L, G_SWIPE_R, G_LONG };

struct gest {
	int ticks, down, long_fired;
	int x0, y0, x1, y1;
	double t0, t_pen;
};

static int gesture_feed(struct gest *g, int pen, int x, int y, double t)
{
	if (pen) {
		g->t_pen = t;
		if (!g->down) {
			if (++g->ticks < 2)
				return G_NONE;
			g->down = 1;
			g->long_fired = 0;
			g->t0 = t;
			g->x0 = g->x1 = x;
			g->y0 = g->y1 = y;
		} else if (x >= 0) {
			if (g->x0 < 0) {
				g->x0 = x;
				g->y0 = y;
			}
			g->x1 = x;
			g->y1 = y;
		}
		if (!g->long_fired && t - g->t0 >= 0.8 &&
		    (g->x0 < 0 || (abs(g->x1 - g->x0) < 40 && abs(g->y1 - g->y0) < 40))) {
			g->long_fired = 1;
			return G_LONG;
		}
		return G_NONE;
	}
	if (t - g->t_pen <= 0.2)
		return G_NONE;
	g->ticks = 0;
	if (!g->down)
		return G_NONE;
	g->down = 0;
	if (g->long_fired)
		return G_NONE;
	if (g->x0 < 0)
		return G_TAP;
	if (abs(g->x1 - g->x0) >= 60 && abs(g->x1 - g->x0) > abs(g->y1 - g->y0))
		return g->x1 < g->x0 ? G_SWIPE_L : G_SWIPE_R;
	return g->x0 < W / 3 ? G_TAP_L : g->x0 >= W * 2 / 3 ? G_TAP_R : G_TAP_M;
}

/* ---------- батарея через PIC16 (I2C 0x2A) ---------- */

/* Статус по разбору прошивки PIC (isublimity/Securifi-Almond-3S): бит 0 -
 * подключено внешнее питание, бит 3 - вывод STAT зарядника BQ24133
 * (1 = не заряжает, заряд окончен), биты 5-6 - флаги смены STAT, оба сразу
 * держатся только без батареи. */
#define BAT_EXT		0x01
#define BAT_IDLE	0x08
#define BAT_NONE	0x60

#ifdef __linux__
#define PIC_ADDR 0x2A
static int pfd = -1;

static int pic_xfer(uint8_t *buf, int len, int rd)
{
	struct i2c_msg m = { .addr = PIC_ADDR, .flags = rd ? I2C_M_RD : 0, .len = len, .buf = buf };
	struct i2c_rdwr_ioctl_data d = { .msgs = &m, .nmsgs = 1 };

	return ioctl(pfd, I2C_RDWR, &d) == 1 ? 0 : -1;
}

/* Заводской протокол: 0x39 (сброс I2C у PIC), 10 мс, 0x36 (свежий замер АЦП),
 * 5 мс, 6 байт [АЦП мл, АЦП ст, статус, ...]. 0 = прочитано, -1 = нет ответа. */
static int bat_read(int *adc, int *stat)
{
	uint8_t c, r[6];

	if (pfd < 0 && (pfd = open("/dev/i2c-0", O_RDWR)) < 0)
		return -1;
	c = 0x39;
	if (pic_xfer(&c, 1, 0))
		return -1;
	usleep(10000);
	c = 0x36;
	if (pic_xfer(&c, 1, 0))
		return -1;
	usleep(6000);
	if (pic_xfer(r, 6, 1) || r[1] > 3)
		return -1;
	*adc = r[1] << 8 | r[0];
	*stat = r[2];
	return 0;
}
#else
static int bat_read(int *adc, int *stat) { (void)adc; (void)stat; return -1; }
#endif

/* Журнал питания на флеше, чтобы пережил и перезагрузку после полного
 * разряда: строка с местным временем на каждую смену состояния батареи
 * (несколько за отключение). Больше 32 КБ - старый уходит в .old. */
#define BAT_LOG_FILE "/etc/almond-lcd.power.log"

static void bat_log(const char *msg)
{
	char ts[24];
	time_t now = time(NULL);
	struct tm tm;
	struct stat sb;
	FILE *f;

	if (!stat(BAT_LOG_FILE, &sb) && sb.st_size > 32768)
		rename(BAT_LOG_FILE, BAT_LOG_FILE ".old");
	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	if ((f = fopen(BAT_LOG_FILE, "a"))) {
		fprintf(f, "%s %s\n", ts, msg);
		fclose(f);
	}
}

/* Кусочно-линейная шкала {АЦП, процент}, точки по убыванию АЦП. */
static int pct_by(const short (*t)[2], int n, int adc)
{
	int i;

	if (adc >= t[0][0])
		return t[0][1];
	for (i = 1; i < n; i++)
		if (adc >= t[i][0])
			return t[i][1] + (adc - t[i][0]) * (t[i - 1][1] - t[i][1]) / (t[i - 1][0] - t[i][0]);
	return 0;
}

/* АЦП под нагрузкой -> процент оставшегося времени работы от батареи.
 * Полный разряд 04.11.2026: от 703 за 4 ч 41 мин, последняя запись 436;
 * ниже ~565 (последние 15 минут) напряжение обваливается. */
static int bat_pct(int adc)
{
	static const short t[][2] = {
		{ 701, 100 }, { 694, 90 }, { 680, 80 }, { 666, 70 }, { 651, 60 }, { 634, 50 },
		{ 620, 40 }, { 607, 30 }, { 597, 20 }, { 577, 10 }, { 563, 5 }, { 544, 3 },
		{ 498, 1 }, { 436, 0 },
	};

	return pct_by(t, sizeof(t) / sizeof(t[0]), adc);
}

/* АЦП на зарядке -> процент заряда. Зарядка 04.11.2026 от полного разряда -
 * 3 ч 32 мин: 2 ч 31 мин ток постоянный и АЦП растёт ровно, потом час стоит
 * на 723-724, пока ток спадает. Считаем, что до плато заряд растёт равномерно
 * по времени и к плато набрано 85 %: при этом АЦП на зарядке на всём пути
 * ровно на ~31 выше, чем при разряде с тем же остатком. На плато точнее не узнать. */
static int chg_pct(int adc)
{
	static const short t[][2] = {
		{ 723, 85 }, { 715, 80 }, { 697, 70 }, { 682, 60 }, { 664, 50 }, { 651, 40 },
		{ 639, 30 }, { 628, 20 }, { 610, 10 }, { 590, 5 }, { 476, 0 },
	};

	return pct_by(t, sizeof(t) / sizeof(t[0]), adc);
}

static void theme_load(void)
{
	char b[16];

	theme_idx = read_file(THEME_FILE, b, sizeof(b)) > 0 && !strncmp(b, "light", 5);
}

static void theme_toggle(void)
{
	FILE *f;

	theme_idx ^= 1;
	shown_valid = 0;		/* перерисовать весь экран */
	if ((f = fopen(THEME_FILE, "w"))) {
		fprintf(f, "%s\n", TH->name);
		fclose(f);
	}
	syslog(LOG_INFO, "theme -> %s", TH->name);
}

/* ---------- медленные проверки от collector ---------- */

struct slow {
	long ts;		/* когда collector писал файл */
	int has_podkop, has_modem;	/* 0 = на роутере нет podkop / модема: индикаторы скрыты */
	char guard[16];		/* ok / warn */
	int podkop_table;	/* 1 = таблица PodkopTable есть, -1 = неизвестно */
	char vpn_ip[48];	/* внешний IP через VPN */
	int fakeip;		/* 1 = FakeIP-проверка podkop прошла */
	int vpn_ok_age;		/* секунд с последней удачной FakeIP-проверки, -1 = не было */
	int ping_ms;		/* до 8.8.8.8 напрямую, -1 = нет ответа */
	int modem_ping;		/* до внешний модем, -1 = нет ответа */
	int lte_ok;		/* статус модема получен */
	char lte_mode[12];	/* LTE / 5G ... */
	int lte_rsrp;		/* dBm */
	double lte_rsrq, lte_sinr;	/* dB */
	int lte_temp;		/* градусы модема, -999 = нет */
	char lte_bands[32];	/* B7+B1+B3+B8 */
	int lte_age;		/* сколько секунд назад модем обновил статус, -1 = нет */
	int lte_sim;		/* встроенный модем: 1 = SIM есть, 0 = нет, -1 = не сообщается */
};

static void slow_read(struct slow *s)
{
	char b[1024], *ln, *save;

	memset(s, 0, sizeof(*s));
	s->ping_ms = -1;
	s->fakeip = -1;
	s->podkop_table = -1;
	s->vpn_ok_age = -1;
	s->modem_ping = -1;
	s->has_podkop = 1;
	s->has_modem = 1;
	s->lte_temp = -999;
	s->lte_age = -1;
	s->lte_sim = -1;
	if (read_file("/tmp/almond-lcd.status", b, sizeof(b)) < 0)
		return;
	for (ln = strtok_r(b, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
		char *v = strchr(ln, '=');

		if (!v)
			continue;
		*v++ = 0;
		if (!strcmp(ln, "ts")) s->ts = strtol(v, NULL, 10);
		else if (!strcmp(ln, "podkop")) s->has_podkop = atoi(v);
		else if (!strcmp(ln, "modem")) s->has_modem = atoi(v);
		else if (!strcmp(ln, "guard")) snprintf(s->guard, sizeof(s->guard), "%s", v);
		else if (!strcmp(ln, "podkop_table")) s->podkop_table = *v ? atoi(v) : -1;
		else if (!strcmp(ln, "vpn_ip")) snprintf(s->vpn_ip, sizeof(s->vpn_ip), "%s", v);
		else if (!strcmp(ln, "fakeip")) s->fakeip = atoi(v);
		else if (!strcmp(ln, "vpn_ok_age")) s->vpn_ok_age = *v ? atoi(v) : -1;
		else if (!strcmp(ln, "ping_ms")) s->ping_ms = *v ? atoi(v) : -1;
		else if (!strcmp(ln, "modem_ping")) s->modem_ping = *v ? atoi(v) : -1;
		else if (!strcmp(ln, "lte_ok")) s->lte_ok = atoi(v);
		else if (!strcmp(ln, "lte_mode")) snprintf(s->lte_mode, sizeof(s->lte_mode), "%s", v);
		else if (!strcmp(ln, "lte_rsrp")) s->lte_rsrp = atoi(v);
		else if (!strcmp(ln, "lte_rsrq")) s->lte_rsrq = strtod(v, NULL);
		else if (!strcmp(ln, "lte_sinr")) s->lte_sinr = strtod(v, NULL);
		else if (!strcmp(ln, "lte_temp")) s->lte_temp = *v ? atoi(v) : -999;
		else if (!strcmp(ln, "lte_bands")) snprintf(s->lte_bands, sizeof(s->lte_bands), "%s", v);
		else if (!strcmp(ln, "lte_age")) s->lte_age = *v ? atoi(v) : -1;
		else if (!strcmp(ln, "lte_sim")) s->lte_sim = *v ? atoi(v) : -1;
	}
}

/* ---------- внешний модем: вывод almond-lcd-m2 для страницы модема ---------- */

/* Скрипт almond-lcd-m2 на модеме (принудительная команда ключа almond-lcd)
 * выводит данные модема строками КЛЮЧ="значение", строки cpu из /proc/stat,
 * память, время работы и wan1. almond-lcd-modem кладёт вывод в M2_FILE
 * целиком раз в 5 с (сборщик - раз в минуту, если поток стоит); со старой
 * командой ключа там только файл сигнала. */
#define M2_FILE "/tmp/almond-lcd.m2"
#define M2_HIST 60		/* точек SINR: по одной в минуту */
#define M2_NCA 6		/* несущих в агрегации */

struct m2_snap {
	int ext;			/* расширенный вывод: есть UPTIME */
	int ncpu;			/* потоков (строки cpu0..) */
	unsigned long long tot[MAXCPU + 1], idle[MAXCPU + 1];	/* [0] - общий */
	long mem_tot, mem_av;		/* кБ, 0 = нет */
	double uptime;			/* с загрузки модема, с (до сотых) */
	int wan_up;			/* 1 / 0, -1 = нет данных */
	long wan_uptime;		/* с подъёма wan1, с */
	char wan_ip[20];
	unsigned long long wan_rx, wan_tx;	/* байты устройства wan1 */
	char cops[16], mode[12];
	long enb;			/* базовая станция, -1 = нет */
	int cell, pci;			/* -1 = нет */
	int nca;
	char ca_band[M2_NCA][6];	/* B7, B20 */
	int ca_mhz[M2_NCA];		/* ширина вниз */
	int rsrp;			/* дБм, 0 = нет */
	double rsrq, sinr;		/* дБ, NAN = нет */
	int temp;			/* градусы, -999 = нет */
	/* встроенный модем (SRC=builtin, вывод almond-lcd-modem по AT: Quectel EC21) */
	int builtin;
	char model[16], fw[20];
	int sim;			/* 1 / 0, -1 = нет данных */
	char reg[18];			/* REGISTERED, SEARCHING, LIMITED ... */
	char band[12];			/* полоса словом: DCS1800, GSM900, B7 */
	char mccmnc[8];
	long lac, cellid;		/* GSM/WCDMA: зона и сота, -1 = нет */
	int arfcn;			/* канал, -1 = нет */
	int rssi;			/* дБм, 0 = нет */
};

struct m2 {
	time_t mtime;			/* когда сборщик записал M2_FILE, 0 = не было */
	int got;			/* в последнем выводе что-то есть (модем ответил) */
	struct m2_snap s;		/* последний вывод */
	int cpu_pct, core_pct[MAXCPU];	/* между двумя последними выводами, -1 = нет */
	double rx_bps, tx_bps;		/* wan1 модема, байт/с, там же; < 0 = нет */
	double sinr_hist[M2_HIST];	/* последнее за минуту, NAN = нет данных */
	double rsrp_hist[M2_HIST];	/* то же для RSRP/RSSI: график встроенного модема без SINR (GSM) */
	long hist_min;			/* минута последней точки (time / 60) */
};

/* Значение после КЛЮЧ= без кавычек и начальных пробелов. */
static void m2_val(const char *v, char *out, size_t n)
{
	size_t l;

	if (*v == '"')
		v++;
	while (*v == ' ')
		v++;
	l = strlen(v);
	if (l && v[l - 1] == '"')
		l--;
	if (l >= n)
		l = n - 1;
	memcpy(out, v, l);
	out[l] = 0;
}

/* Число в начале строки ("13 dB", "-13.5") или NAN. */
static double m2_num(const char *v)
{
	char *end;
	double d = strtod(v, &end);

	return end != v ? d : NAN;
}

/* LBAND: "B7 (Bandwidth: 10 MHz)<br />B1 (CA, Bandwidth: 15 MHz down | 5 MHz up)<br />..." */
static void m2_bands(struct m2_snap *s, const char *v)
{
	while (*v && s->nca < M2_NCA) {
		const char *next = strstr(v, "<br />"), *bw;
		int k = 0;

		while (*v == ' ')
			v++;
		while (v[k] && v[k] != ' ' && v[k] != '<' && k < 5)
			k++;
		bw = strstr(v, "Bandwidth: ");
		if (*v == 'B' && k > 1) {
			memcpy(s->ca_band[s->nca], v, k);
			s->ca_band[s->nca][k] = 0;
			s->ca_mhz[s->nca] = bw && (!next || bw < next) ? atoi(bw + 11) : 0;
			s->nca++;
		}
		if (!next)
			break;
		v = next + 6;
	}
}

/* CID: "4G Short 6 (6), Long F25DF06 (254140166)<br />5G ..." - первый длинный
 * (4G): базовая станция = CID >> 8, сота = CID & 255. */
static void m2_cid(struct m2_snap *s, const char *v)
{
	const char *p = strstr(v, "Long ");
	long cid;

	if (p && (p = strchr(p, '(')) && (cid = strtol(p + 1, NULL, 10)) > 0) {
		s->enb = cid >> 8;
		s->cell = (int)(cid & 255);
	}
}

/* Разбор вывода модема. 1 = в нём что-то есть, 0 = пусто (модем не ответил). */
static int m2_parse(const char *buf, struct m2_snap *s)
{
	char b[4096], v[512], *ln, *save;
	int any = 0;

	memset(s, 0, sizeof(*s));
	s->wan_up = -1;
	s->enb = -1;
	s->cell = s->pci = -1;
	s->rsrq = s->sinr = NAN;
	s->temp = -999;
	s->sim = -1;
	s->lac = s->cellid = -1;
	s->arfcn = -1;
	snprintf(b, sizeof(b), "%s", buf);
	for (ln = strtok_r(b, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
		unsigned long long t, i;
		char *eq;
		double d;
		int idx;

		any = 1;
		if ((idx = cpu_line(ln, &t, &i)) >= 0) {
			s->tot[idx] = t;
			s->idle[idx] = i;
			if (idx > s->ncpu)
				s->ncpu = idx;
			continue;
		}
		if (!(eq = strchr(ln, '=')))
			continue;
		*eq = 0;
		m2_val(eq + 1, v, sizeof(v));
		if (!strcmp(ln, "UPTIME")) s->ext = 1, s->uptime = strtod(v, NULL);
		else if (!strcmp(ln, "MEM_TOTAL")) s->mem_tot = atol(v);
		else if (!strcmp(ln, "MEM_AVAIL")) s->mem_av = atol(v);
		else if (!strcmp(ln, "WAN_UP")) s->wan_up = *v ? atoi(v) : -1;
		else if (!strcmp(ln, "WAN_UPTIME")) s->wan_uptime = atol(v);
		else if (!strcmp(ln, "WAN_IP")) m2_val(eq + 1, s->wan_ip, sizeof(s->wan_ip));
		else if (!strcmp(ln, "WAN_RX")) s->wan_rx = strtoull(v, NULL, 10);
		else if (!strcmp(ln, "WAN_TX")) s->wan_tx = strtoull(v, NULL, 10);
		else if (!strcmp(ln, "COPS")) m2_val(eq + 1, s->cops, sizeof(s->cops));
		else if (!strcmp(ln, "MODE")) m2_val(eq + 1, s->mode, sizeof(s->mode));
		else if (!strcmp(ln, "PCI")) s->pci = isdigit((unsigned char)*v) ? atoi(v) : -1;
		else if (!strcmp(ln, "CID")) m2_cid(s, v);
		else if (!strcmp(ln, "LBAND")) m2_bands(s, v);
		else if (!strcmp(ln, "RSCP")) s->rsrp = atoi(v);
		else if (!strcmp(ln, "ECIO")) s->rsrq = m2_num(v);
		else if (!strcmp(ln, "SINR")) s->sinr = m2_num(v);
		else if (!strcmp(ln, "TEMP")) s->temp = isnan(d = m2_num(v)) ? -999 : (int)d;
		else if (!strcmp(ln, "SRC")) s->builtin = !strcmp(v, "builtin");
		else if (!strcmp(ln, "MODEL")) m2_val(eq + 1, s->model, sizeof(s->model));
		else if (!strcmp(ln, "FW")) m2_val(eq + 1, s->fw, sizeof(s->fw));
		else if (!strcmp(ln, "SIM")) s->sim = isdigit((unsigned char)*v) ? atoi(v) : -1;
		else if (!strcmp(ln, "REG")) m2_val(eq + 1, s->reg, sizeof(s->reg));
		else if (!strcmp(ln, "BAND")) m2_val(eq + 1, s->band, sizeof(s->band));
		else if (!strcmp(ln, "MCCMNC")) m2_val(eq + 1, s->mccmnc, sizeof(s->mccmnc));
		/* ниже - только для встроенного модема (SRC=builtin идёт первой строкой): у других источников
		 * ключ LAC тоже есть, но текстом ("4G 0D95 (3477), ..."), его не трогаем */
		else if (!s->builtin) ;
		else if (!strcmp(ln, "LAC")) s->lac = isdigit((unsigned char)*v) ? atol(v) : -1;
		else if (!strcmp(ln, "CELLID")) s->cellid = isdigit((unsigned char)*v) ? atol(v) : -1;
		else if (!strcmp(ln, "ARFCN")) s->arfcn = isdigit((unsigned char)*v) ? atoi(v) : -1;
		else if (!strcmp(ln, "RSSI")) s->rssi = atoi(v);
	}
	return any;
}

/* Загрузка в процентах по двум снимкам счётчиков, -1 - не посчитать. */
static int busy_pct(unsigned long long t1, unsigned long long i1,
		    unsigned long long t0, unsigned long long i0)
{
	if (!t0 || t1 <= t0 || i1 < i0 || i1 - i0 > t1 - t0)
		return -1;
	return (int)(100 - (i1 - i0) * 100 / (t1 - t0));
}

static void m2_init(struct m2 *m)
{
	int i;

	memset(m, 0, sizeof(*m));
	m->cpu_pct = -1;
	for (i = 0; i < MAXCPU; i++)
		m->core_pct[i] = -1;
	m->rx_bps = m->tx_bps = -1;
	for (i = 0; i < M2_HIST; i++)
		m->sinr_hist[i] = m->rsrp_hist[i] = NAN;
}

/* Новый вывод модема (now - когда записан): загрузка процессора и скорость
 * wan1 по разнице с прошлым выводом (время - по часам модема, до сотых), SINR
 * - последнее значение за минуту; минута без значения - разрыв графика. */
static void m2_apply(struct m2 *m, const char *buf, time_t now)
{
	struct m2_snap s;
	double dt;
	int i;

	m->got = m2_parse(buf, &s);
	m->cpu_pct = busy_pct(s.tot[0], s.idle[0], m->s.tot[0], m->s.idle[0]);
	for (i = 0; i < MAXCPU; i++)
		m->core_pct[i] = i < s.ncpu ?
			busy_pct(s.tot[i + 1], s.idle[i + 1], m->s.tot[i + 1], m->s.idle[i + 1]) : -1;
	dt = s.uptime - m->s.uptime;
	if (s.ext && m->s.ext && dt > 0 && m->s.wan_rx && s.wan_rx >= m->s.wan_rx && s.wan_tx >= m->s.wan_tx) {
		m->rx_bps = (s.wan_rx - m->s.wan_rx) / dt;
		m->tx_bps = (s.wan_tx - m->s.wan_tx) / dt;
	} else {
		m->rx_bps = m->tx_bps = -1;
	}
	if (now / 60 != m->hist_min) {
		memmove(m->sinr_hist, m->sinr_hist + 1, (M2_HIST - 1) * sizeof(m->sinr_hist[0]));
		memmove(m->rsrp_hist, m->rsrp_hist + 1, (M2_HIST - 1) * sizeof(m->rsrp_hist[0]));
		m->sinr_hist[M2_HIST - 1] = m->rsrp_hist[M2_HIST - 1] = NAN;
		m->hist_min = now / 60;
	}
	if (!isnan(s.sinr))
		m->sinr_hist[M2_HIST - 1] = s.sinr;
	if (s.rsrp)
		m->rsrp_hist[M2_HIST - 1] = s.rsrp;
	m->s = s;
}

/* Раз в секунду: сборщик переписал M2_FILE - разобрать заново. */
static void m2_update(struct m2 *m)
{
	struct stat sb;
	char b[4096];

	if (stat(M2_FILE, &sb) || sb.st_mtime == m->mtime)
		return;
	m->mtime = sb.st_mtime;
	if (read_file(M2_FILE, b, sizeof(b)) < 0)
		b[0] = 0;
	m2_apply(m, b, sb.st_mtime);
}

static void fmt_rate(char *out, size_t n, double bps)
{
	double mbit = bps * 8 / 1e6;

	if (mbit >= 100)
		snprintf(out, n, "%.0f Mbit/s", mbit);
	else if (mbit >= 1)
		snprintf(out, n, "%.1f Mbit/s", mbit);
	else
		snprintf(out, n, "%.0f kbit/s", bps * 8 / 1e3);
}

/* ---------- вывод на панель ---------- */

static int flush(int fd)
{
	int r = 0;

	while (r < H) {
		int r1;
		size_t len;

		if (shown_valid && !memcmp(fb[r], shown[r], sizeof(fb[r]))) {
			r++;
			continue;
		}
		for (r1 = r; r1 + 1 < H && !(shown_valid && !memcmp(fb[r1 + 1], shown[r1 + 1], sizeof(fb[r1 + 1]))); r1++)
			;
		len = (size_t)(r1 - r + 1) * W * 2;
		if (pwrite(fd, fb[r], len, (off_t)r * W * 2) != (ssize_t)len)
			return -1;
		memcpy(shown[r], fb[r], len);
		r = r1 + 1;
	}
	shown_valid = 1;
	return 0;
}

/* ---------- состояние и экран ---------- */

struct state {
	int cpu_pct;
	int ncpu, core_pct[MAXCPU];
	long mem_tot, mem_av;
	double rx, tx;			/* WAN, байт/с */
	double rx_hist[HIST], tx_hist[HIST];
	int sb_alive, tun_alive;
	int tun_cfg;			/* на роутере замечен обратный SSH-туннель: иначе TUN не показываем */
	struct slow s;
	int slow_fresh;
	struct wifi wf[2];		/* 0 = 2.4 ГГц, 1 = 5 ГГц */
	struct wifi wanw;		/* WAN (через модем): активность для стрелок */
	double rx_idle;			/* сколько секунд по WAN не приходило пакетов */
	char loadavg[24];
	double uptime;
	int phase;			/* чётность секунды: стрелки переливаются раз в секунду */
	int bat_ok;			/* 1 = PIC отвечает, статус подтверждён повторным чтением */
	int bat_adc, bat_stat;
	int page;			/* 0 = Almond, 1 = модем */
	struct m2 m2;			/* модем: последний вывод almond-lcd-m2 */
};

/* При трафике стрелка всегда зелёная и раз в секунду переливается между
 * ярким и обычным зелёным; без трафика - серая. */
static uint16_t arrow_color(int active, int phase)
{
	return active ? ((phase & 1) ? C_OK : C_LIT) : C_IDLE;
}

/* Полоски сигнала по RSRP. */
static int lte_bars(int rsrp)
{
	return rsrp >= -85 ? 4 : rsrp >= -95 ? 3 : rsrp >= -105 ? 2 : rsrp >= -115 ? 1 : 0;
}

/* Общий статус мобильной сети. */
enum { M_UNKNOWN, M_DOWN, M_NOSTATUS, M_NODATA, M_NONET, M_WEAK, M_ONLINE, M_NOSIM };

static int modem_state(const struct state *st)
{
	const struct slow *s = &st->s;

	if (st->rx_idle >= 10)
		return M_NODATA;		/* ответные пакеты не приходят - видно сразу */
	if (!st->slow_fresh || !s->has_modem)
		return M_UNKNOWN;
	if (s->modem_ping < 0)
		return M_DOWN;
	if (!s->lte_ok)
		return M_NOSTATUS;
	if (s->lte_sim == 0)
		return M_NOSIM;		/* встроенный модем без SIM: интернет по кабелю, это норма */
	if (s->ping_ms < 0)
		return M_NONET;
	if (lte_bars(s->lte_rsrp) <= 1 || s->lte_sinr < 0)
		return M_WEAK;
	return M_ONLINE;
}

/* podkop: 2 = всё хорошо, 1 = частично, 0 = сломан, -1 = неизвестно. */
static int podkop_state(const struct state *st)
{
	const struct slow *s = &st->s;

	if (!st->slow_fresh || !s->has_podkop)
		return -1;
	if (s->podkop_table == 0)
		return 0;
	/* одиночный сбой внешней проверки не в счёт: жёлтый, только если
	 * удачной проверки не было дольше 3 минут */
	if (s->vpn_ok_age >= 0 && s->vpn_ok_age <= 180 && !strcmp(s->guard, "ok"))
		return 2;
	return 1;
}

static void signal_bars(int x, int ybot, int bars, uint16_t c)
{
	int i;

	for (i = 0; i < 4; i++)
		fill(x + i * 6, ybot - 4 * (i + 1), 4, 4 * (i + 1), i < bars ? c : C_TRACK);
}

/* Работает от батареи: внешнего питания нет, батарея на месте. */
static int on_battery(const struct state *st)
{
	return st->bat_ok && !(st->bat_stat & BAT_EXT) && (st->bat_stat & BAT_NONE) != BAT_NONE;
}

/* Идёт зарядка: внешнее питание есть, заряд не окончен. */
static int charging(const struct state *st)
{
	return st->bat_ok && (st->bat_stat & (BAT_EXT | BAT_IDLE)) == BAT_EXT &&
	       (st->bat_stat & BAT_NONE) != BAT_NONE;
}

/* Батарейка 42x14 с носиком, 5 делений по 20 %. От сети и заряд окончен -
 * полная зелёная; идёт зарядка - залиты набранные 20 % по шкале зарядки,
 * следующее деление мигает; от батареи - жёлтая, на последнем делении
 * (меньше часа) красная, последние ~15 минут красное мигает. Нет батареи
 * или PIC молчит - пустой контур. */
static void battery_icon(int x, int y, const struct state *st)
{
	int bw = 42, bh = 14, n = 0, blink = -1, k, pct;
	uint16_t c = C_DIM;

	if (st->bat_ok && (st->bat_stat & BAT_NONE) != BAT_NONE) {
		pct = bat_pct(st->bat_adc);
		n = (pct + 19) / 20;
		if (!(st->bat_stat & BAT_EXT))
			c = n <= 1 ? C_BAD : C_WARN, blink = pct < 5 ? 0 : -1;
		else if (st->bat_stat & BAT_IDLE)
			n = 5, c = C_OK;
		else
			n = chg_pct(st->bat_adc) / 20, c = C_OK, blink = n < 5 ? n : 4;
	}
	fill(x, y, bw, 1, C_DIM);
	fill(x, y + bh - 1, bw, 1, C_DIM);
	fill(x, y, 1, bh, C_DIM);
	fill(x + bw - 1, y, 1, bh, C_DIM);
	fill(x + bw, y + 4, 3, bh - 8, C_DIM);
	for (k = 0; k < 5; k++)
		fill(x + 2 + k * 8, y + 2, 6, bh - 4,
		     k == blink ? ((st->phase & 1) ? c : C_TRACK) : k < n ? c : C_TRACK);
}

/* Точка-индикатор и подпись; возвращает x после подписи. */
static int pill(int x, int y, uint16_t c, const char *label)
{
	dot(x + 4, y + 8, 4, c);
	return text(x + 12, y, label, C_TEXT, 1) + 10;
}

/* Время работы: 2d05h или 3h07m. */
static void fmt_uptime(char *out, size_t n, long sec)
{
	if (sec >= 86400)
		snprintf(out, n, "%ldd%02ldh", sec / 86400, sec / 3600 % 24);
	else
		snprintf(out, n, "%ldh%02ldm", sec / 3600, sec / 60 % 60);
}

/* Общий статус для шапки обеих страниц. */
static uint16_t overall_color(const struct state *st)
{
	const struct slow *s = &st->s;
	int net_ok = st->slow_fresh && s->ping_ms >= 0;
	int ms = modem_state(st), pk = podkop_state(st);

	if ((s->has_podkop && !st->sb_alive) || (st->slow_fresh && !net_ok) || pk == 0 ||
	    ms == M_DOWN || ms == M_NODATA || ms == M_NONET)
		return C_BAD;
	if (!st->slow_fresh || (s->has_podkop && pk != 2) || (st->tun_cfg && !st->tun_alive) ||
	    (s->has_modem && ms != M_ONLINE && ms != M_NOSIM) || on_battery(st))
		return C_WARN;
	return C_OK;
}

/* Шапка: общий статус точкой и полосой, название, по центру часы с мигающей
 * точкой; правый край заполняет страница. */
static void header(const char *title, uint16_t overall)
{
	char b[16];
	time_t now = time(NULL);
	struct tm tm;

	fill(0, 0, W, 26, C_PANEL);
	fill(0, 24, W, 2, overall);
	dot(12, 12, 6, overall);
	text(24, 5, title, C_TEXT, 1);
	localtime_r(&now, &tm);
	strftime(b, sizeof(b), "%H:%M:%S", &tm);
	text((W - text_w(b, 1)) / 2, 5, b, C_TEXT, 1);
	if (now & 1)
		dot((W - text_w(b, 1)) / 2 - 8, 12, 3, C_OK);
}

static void render(const struct state *st)
{
	char b[64], r1[24], r2[24];
	int i, mem_pct = st->mem_tot ? (int)((st->mem_tot - st->mem_av) * 100 / st->mem_tot) : 0;
	const struct slow *s = &st->s;
	int ms = modem_state(st), pk = podkop_state(st);
	double mx = 1;

	fill(0, 0, W, H, C_BG);
	header("ALMOND 3S", overall_color(st));
	battery_icon(W - 8 - 3 - 42, 5, st);

	/* CPU: подпись, крупный процент, полоса, load average под ней */
	text(8, 36, "CPU", C_DIM, 1);
	snprintf(b, sizeof(b), "%d%%", st->cpu_pct);
	text(44, 28, b, C_TEXT, 2);
	bar(124, 32, W - 132, 10, st->cpu_pct, level_color(st->cpu_pct, 60, 85));
	/* полоски отдельных ядер (у MT7621 - 2 ядра x 2 потока) */
	if (st->ncpu > 0) {
		int gap = 6, cw = (W - 132 - gap * (st->ncpu - 1)) / st->ncpu, k;

		for (k = 0; k < st->ncpu; k++)
			bar(124 + k * (cw + gap), 48, cw, 6, st->core_pct[k],
			    level_color(st->core_pct[k], 60, 85));
	}

	/* RAM: то же, под полосой занято/всего */
	text(8, 70, "RAM", C_DIM, 1);
	snprintf(b, sizeof(b), "%d%%", mem_pct);
	text(44, 62, b, C_TEXT, 2);
	bar(124, 66, W - 132, 10, mem_pct, level_color(mem_pct, 90, 95));
	snprintf(b, sizeof(b), "%ld / %ld MB", (st->mem_tot - st->mem_av) / 1024, st->mem_tot / 1024);
	text(124, 78, b, C_DIM, 1);

	/* WAN: скорости и график за 2 минуты */
	text(8, 100, "WAN", C_DIM, 1);
	fmt_rate(r1, sizeof(r1), st->rx);
	fmt_rate(r2, sizeof(r2), st->tx);
	text(8, 118, S_DN, C_RX, 1);
	text(20, 118, r1, C_RX, 1);
	text(8, 136, S_UP, C_TX, 1);
	text(20, 136, r2, C_TX, 1);
	{
		int gx = 112, gy = 98, gw = W - 8 - 112, gh = 56, x;

		fill(gx, gy, gw, gh, C_PANEL);
		for (x = 1; x < 4; x++)
			fill(gx, gy + gh * x / 4, gw, 1, C_GRID);
		for (i = 0; i < HIST; i++) {
			if (st->rx_hist[i] > mx) mx = st->rx_hist[i];
			if (st->tx_hist[i] > mx) mx = st->tx_hist[i];
		}
		for (i = 1; i < HIST; i++) {
			int x0 = gx + (i - 1) * gw / HIST, x1 = gx + i * gw / HIST;

			line(x0, gy + gh - 1 - (int)(st->tx_hist[i - 1] / mx * (gh - 2)),
			     x1, gy + gh - 1 - (int)(st->tx_hist[i] / mx * (gh - 2)), C_TX);
			line(x0, gy + gh - 1 - (int)(st->rx_hist[i - 1] / mx * (gh - 2)),
			     x1, gy + gh - 1 - (int)(st->rx_hist[i] / mx * (gh - 2)), C_RX);
		}
		fmt_rate(b, sizeof(b), mx);
		text_r(gx + gw - 2, gy + 1, b, C_DIM, 1);
	}

	/* Wi-Fi: значок, по диапазону подпись, стрелки приём/отдача, устройства */
	{
		int any_up = st->wf[0].up || st->wf[1].up, k;

		fill(0, 160, W, 26, C_PANEL);
		wifi_icon(20, 180, any_up ? C_OK : C_BAD);
		for (k = 0; k < 2; k++) {
			const struct wifi *w = &st->wf[k];
			int x = 44 + k * 140;

			text(x, 165, w->label, w->up ? C_TEXT : C_BAD, 1);
			x += text_w(w->label, 1) + 6;
			if (!w->up) {
				text(x, 165, "OFF", C_BAD, 1);
				continue;
			}
			text(x, 165, S_DN, arrow_color(w->rx_act, st->phase), 1);
			text(x + 9, 165, S_UP, arrow_color(w->tx_act, st->phase), 1);
			if (w->clients >= 0) {
				snprintf(b, sizeof(b), "%d", w->clients);
				text(x + 24, 165, b, C_DIM, 1);
			}
		}
	}

	/* podkop, sing-box, ping, туннель агента, аптайм */
	{
		int x = 6;
		uint16_t pc = pk == 2 ? C_OK : pk == 1 ? C_WARN : pk == 0 ? C_BAD : C_DIM;
		uint16_t gc;

		fill(0, 190, W, 26, C_PANEL);
		if (s->has_podkop) {
			x = pill(x, 195, pc, "PODKOP");
			x = pill(x, 195, st->sb_alive ? C_OK : C_BAD, "SING-BOX");
		}
		if (!st->slow_fresh) {
			gc = C_DIM;
			snprintf(b, sizeof(b), "--");
		} else if (s->ping_ms < 0) {
			gc = C_BAD;
			snprintf(b, sizeof(b), "NET!");
		} else {
			gc = s->ping_ms < 150 ? C_OK : C_WARN;
			snprintf(b, sizeof(b), "%dms", s->ping_ms);
		}
		x = pill(x, 195, gc, b);
		if (st->tun_cfg)
			pill(x, 195, st->tun_alive ? C_OK : C_BAD, "TUN");
		fmt_uptime(b, sizeof(b), (long)st->uptime);
		text_r(W - 6, 195, b, C_DIM, 1);
	}

	/* Мобильная сеть (внешний модем): статус словом, стрелки WAN, диапазоны, температура */
	{
		static const char *const word[] = {
			"--", "MODEM DOWN", "NO STATUS", "NO DATA", "NO NET", "WEAK", "ONLINE", "NO SIM"
		};
		uint16_t mc = ms == M_ONLINE ? C_OK : ms == M_WEAK || ms == M_NOSTATUS ? C_WARN :
			      ms == M_UNKNOWN || ms == M_NOSIM ? C_DIM : C_BAD;
		int bars = s->lte_ok ? lte_bars(s->lte_rsrp) : 0;
		int x;

		fill(0, 218, W, 22, C_PANEL);
		if (!s->has_modem) {	/* модема нет: только состояние WAN */
			uint16_t nc = !st->slow_fresh ? C_DIM : s->ping_ms >= 0 ? C_OK : C_BAD;

			x = text(8, 221, "WAN", C_TEXT, 1) + 8;
			x = text(x, 221, !st->slow_fresh ? "--" : s->ping_ms >= 0 ? "ONLINE" : "NO NET", nc, 1) + 8;
			text(x, 221, S_DN, arrow_color(st->wanw.rx_act, st->phase), 1);
			text(x + 9, 221, S_UP, arrow_color(st->wanw.tx_act, st->phase), 1);
			return;
		}
		signal_bars(8, 236, ms == M_ONLINE || ms == M_WEAK || ms == M_NOSIM ? bars : 4, mc);
		x = text(40, 221, s->lte_ok && s->lte_mode[0] ? s->lte_mode : "LTE", C_TEXT, 1) + 8;
		x = text(x, 221, word[ms], mc, 1) + 8;
		text(x, 221, S_DN, arrow_color(st->wanw.rx_act, st->phase), 1);
		text(x + 9, 221, S_UP, arrow_color(st->wanw.tx_act, st->phase), 1);
		if (s->lte_ok && st->slow_fresh) {
			char bands[40];
			const char *src = s->lte_bands;
			int j = 0;

			/* B7+B1+B3 -> B7+1+3 */
			for (; *src && j < (int)sizeof(bands) - 1; src++)
				if (!(*src == 'B' && src != s->lte_bands))
					bands[j++] = *src;
			bands[j] = 0;
			if (x + 26 <= 204)
				text(204, 221, bands, C_DIM, 1);
			if (s->lte_temp > -999) {
				snprintf(b, sizeof(b), "%d" S_DEG "C", s->lte_temp);
				text_r(W - 8, 221, b, s->lte_temp >= 70 ? C_BAD : s->lte_temp >= 60 ? C_WARN : C_DIM, 1);
			}
		}
	}
}

/* Вилка питания модема 11x15. */
static void plug_icon(int x, int y, uint16_t c)
{
	fill(x + 2, y, 2, 4, c);
	fill(x + 7, y, 2, 4, c);
	fill(x, y + 4, 11, 6, c);
	fill(x + 1, y + 10, 9, 1, c);
	fill(x + 4, y + 11, 3, 4, c);
}

static uint16_t sig_color(double v, double ok, double warn)
{
	return v >= ok ? C_OK : v >= warn ? C_WARN : C_BAD;
}

/* SINR на графике: 0..30 дБ на высоту gh. */
static int sinr_y(double v, int gh)
{
	if (v < 0) v = 0;
	if (v > 30) v = 30;
	return (int)(v * (gh - 4) / 30);
}

/* RSRP/RSSI на графике: -115..-55 дБм на высоту gh. */
static int rsrp_y(double v, int gh)
{
	if (v < -115) v = -115;
	if (v > -55) v = -55;
	return (int)((v + 115) * (gh - 4) / 60);
}

/* Вторая страница: внешний модем по последнему выводу almond-lcd-m2. Вывод
 * старше 3 минут - всё серое; чего нет в выводе - прочерк. */
static void render_modem(const struct state *st)
{
	const struct m2 *m = &st->m2;
	const struct m2_snap *s = &m->s;
	int fresh = m->mtime && time(NULL) - m->mtime < 180;
	int mem_pct = s->mem_tot > 0 ? (int)((s->mem_tot - s->mem_av) * 100 / s->mem_tot) : -1;
	int i, x, total = 0;
	char b[48], b2[24];
	uint16_t pc;
#define LV(c) (fresh ? (c) : C_DIM)

	fill(0, 0, W, H, C_BG);
	header(s->model[0] ? s->model : "MODEM", overall_color(st));

	/* питание: зелёная вилка - модем ответил, красная - молчит и не пингуется;
	 * рядом сколько модем включён (сброс - значит, пропадало питание) */
	pc = fresh && (s->ext || (s->builtin && m->got)) ? C_OK :
	     fresh && !m->got && st->slow_fresh && st->s.modem_ping < 0 ? C_BAD : C_DIM;
	if (s->ext)
		fmt_uptime(b, sizeof(b), (long)s->uptime);
	else
		snprintf(b, sizeof(b), "--");
	plug_icon(W - 8 - (text_w(b, 1) > 40 ? text_w(b, 1) : 40) - 18, 5, pc);
	text_r(W - 8, 5, b, pc == C_OK ? C_TEXT : pc, 1);

	if (s->builtin) {
		/* встроенный модем: SIM и регистрация, сеть и прошивка (CPU и RAM у него не спросить) */
		int reg_ok = s->sim == 1 && !strncmp(s->reg, "REG", 3);

		/* крупный шрифт (масштаб 2) только цифровой, поэтому слова - обычным */
		text(8, 36, "SIM", C_DIM, 1);
		text(44, 30, s->sim == 0 ? "NO SIM" : s->sim == 1 ? "READY" : "--",
		     s->sim == 0 ? LV(C_WARN) : s->sim == 1 ? LV(C_OK) : C_DIM, 1);
		text(152, 30, s->reg[0] ? s->reg : "--", s->reg[0] ? LV(reg_ok ? C_OK : C_TEXT) : C_DIM, 1);
		for (i = 0; s->cops[i] && i < (int)sizeof(b) - 1; i++)
			b[i] = (char)toupper((unsigned char)s->cops[i]);
		b[i] = 0;
		x = text(152, 46, b[0] ? b : "--", C_DIM, 1) + 8;
		if (s->mccmnc[0])
			text(x, 46, s->mccmnc, C_DIM, 1);
		if (s->temp > -999) {
			snprintf(b, sizeof(b), "%d" S_DEG "C", s->temp);
			text_r(W - 8, 46, b, LV(s->temp >= 70 ? C_BAD : s->temp >= 60 ? C_WARN : C_DIM), 1);
		}

		text(8, 70, "NET", C_DIM, 1);
		text(44, 62, s->mode[0] ? s->mode : "--", LV(C_TEXT), 1);
		text(152, 62, s->band[0] ? s->band : "--", LV(C_TEXT), 1);
		if (s->fw[0])
			text(152, 78, s->fw, C_DIM, 1);
	} else {
	/* CPU: за минуту между двумя выводами, полоски 4 потоков */
	text(8, 36, "CPU", C_DIM, 1);
	if (m->cpu_pct >= 0)
		snprintf(b, sizeof(b), "%d%%", m->cpu_pct);
	else
		snprintf(b, sizeof(b), "--");
	text(44, 28, b, LV(C_TEXT), 2);
	bar(124, 32, W - 132, 10, m->cpu_pct, LV(level_color(m->cpu_pct, 60, 85)));
	if (s->ncpu > 0) {
		int gap = 6, cw = (W - 132 - gap * (s->ncpu - 1)) / s->ncpu;

		for (i = 0; i < s->ncpu; i++)
			bar(124 + i * (cw + gap), 48, cw, 6, m->core_pct[i],
			    LV(level_color(m->core_pct[i], 60, 85)));
	}

	/* RAM и температура модема */
	text(8, 70, "RAM", C_DIM, 1);
	if (mem_pct >= 0)
		snprintf(b, sizeof(b), "%d%%", mem_pct);
	else
		snprintf(b, sizeof(b), "--");
	text(44, 62, b, LV(C_TEXT), 2);
	bar(124, 66, W - 132, 10, mem_pct, LV(level_color(mem_pct, 90, 95)));
	if (s->mem_tot > 0) {
		snprintf(b, sizeof(b), "%ld / %ld MB", (s->mem_tot - s->mem_av) / 1024, s->mem_tot / 1024);
		text(124, 78, b, C_DIM, 1);
	}
	if (s->temp > -999) {
		snprintf(b, sizeof(b), "%d" S_DEG "C", s->temp);
		text_r(W - 8, 78, b, LV(s->temp >= 70 ? C_BAD : s->temp >= 60 ? C_WARN : C_DIM), 1);
	}
	}

	/* сигнал: значения слева, SINR за час справа (без SINR, как у GSM, - RSRP/RSSI за час) */
	text(8, 100, s->builtin && strcmp(s->mode, "LTE") ? "RSSI" : "RSRP", C_DIM, 1);
	text(8, 118, "RSRQ", C_DIM, 1);
	text(8, 136, "SINR", C_DIM, 1);
	if (s->rsrp)
		snprintf(b, sizeof(b), "%d", s->rsrp);
	else
		snprintf(b, sizeof(b), "--");
	text_r(104, 100, b, s->rsrp ? LV(sig_color(s->rsrp, -95, -105)) : C_DIM, 1);
	if (!isnan(s->rsrq))
		snprintf(b, sizeof(b), "%g", s->rsrq);
	else
		snprintf(b, sizeof(b), "--");
	text_r(104, 118, b, !isnan(s->rsrq) ? LV(sig_color(s->rsrq, -15, -18)) : C_DIM, 1);
	if (!isnan(s->sinr))
		snprintf(b, sizeof(b), "%g", s->sinr);
	else
		snprintf(b, sizeof(b), "--");
	text_r(104, 136, b, !isnan(s->sinr) ? LV(sig_color(s->sinr, 10, 3)) : C_DIM, 1);
	{
		int gx = 112, gy = 98, gw = W - 8 - 112, gh = 56;

		fill(gx, gy, gw, gh, C_PANEL);
		for (i = 1; i < 4; i++)
			fill(gx, gy + gh * i / 4, gw, 1, C_GRID);
		int use_rsrp = s->builtin && isnan(s->sinr);
		const double *hist = use_rsrp ? m->rsrp_hist : m->sinr_hist;

		for (i = 1; i < M2_HIST; i++) {
			double a = hist[i - 1], c = hist[i];

			if (isnan(a) || isnan(c))
				continue;
			line(gx + (i - 1) * (gw - 1) / (M2_HIST - 1), gy + gh - 2 - (use_rsrp ? rsrp_y(a, gh) : sinr_y(a, gh)),
			     gx + i * (gw - 1) / (M2_HIST - 1), gy + gh - 2 - (use_rsrp ? rsrp_y(c, gh) : sinr_y(c, gh)), LV(C_RX));
		}
		text_r(gx + gw - 2, gy + 1, use_rsrp ? (strcmp(s->mode, "LTE") ? "RSSI 1h" : "RSRP 1h") : "SINR 1h", C_DIM, 1);
	}

	/* агрегация: режим, несущие с шириной вниз (основная - цветом текста), сумма */
	fill(0, 160, W, 26, C_PANEL);
	x = text(8, 165, s->mode[0] ? s->mode : "--", LV(C_TEXT), 1) + 8;
	for (i = 0; i < s->nca; i++)
		total += s->ca_mhz[i];
	for (i = 0; i < s->nca; i++) {
		snprintf(b, sizeof(b), "%s\x83%d", s->ca_band[i], s->ca_mhz[i]);
		if (x + text_w(b, 1) > W - 8 - 48 - 8)
			break;
		x = text(x, 165, b, i ? C_DIM : LV(C_TEXT), 1) + 8;
	}
	if (s->builtin && !s->nca && s->band[0])
		x = text(x, 165, s->band, LV(C_TEXT), 1) + 8;
	if (total) {
		snprintf(b, sizeof(b), "%dMHz", total);
		text_r(W - 8, 165, b, C_DIM, 1);
	} else if (s->builtin && s->arfcn >= 0) {
		snprintf(b, sizeof(b), "ARFCN %d", s->arfcn);
		text_r(W - 8, 165, b, C_DIM, 1);
	}

	/* сота: оператор, базовая станция и сота, PCI основной несущей */
	fill(0, 190, W, 26, C_PANEL);
	for (i = 0; s->cops[i] && i < (int)sizeof(b) - 1; i++)
		b[i] = (char)toupper((unsigned char)s->cops[i]);
	b[i] = 0;
	x = text(8, 195, b[0] ? b : "--", LV(C_TEXT), 1) + 12;
	if (s->enb >= 0) {
		x = text(x, 195, "eNB", C_DIM, 1) + 6;
		snprintf(b, sizeof(b), "%ld-%d", s->enb, s->cell);
		x = text(x, 195, b, LV(C_TEXT), 1) + 12;
	}
	if (s->pci >= 0) {
		x = text(x, 195, "PCI", C_DIM, 1) + 6;
		snprintf(b, sizeof(b), "%d", s->pci);
		x = text(x, 195, b, LV(C_TEXT), 1) + 12;
	}
	if (s->builtin && s->enb < 0 && s->cellid >= 0) {	/* GSM/WCDMA: зона и сота */
		if (s->lac >= 0) {
			x = text(x, 195, "LAC", C_DIM, 1) + 6;
			snprintf(b, sizeof(b), "%ld", s->lac);
			x = text(x, 195, b, LV(C_TEXT), 1) + 12;
		}
		x = text(x, 195, "CI", C_DIM, 1) + 6;
		snprintf(b, sizeof(b), "%ld", s->cellid);
		text(x, 195, b, LV(C_TEXT), 1);
	}

	/* подключение wan1: скорость между двумя выводами (раз в 5 с), сколько держится */
	fill(0, 218, W, 22, C_PANEL);
	if (s->builtin && s->sim == 0) {
		text(8, 221, "NO SIM: WAN BY CABLE", C_DIM, 1);
	} else if (s->wan_up == 0) {
		text(8, 221, "WAN DOWN", LV(C_BAD), 1);
	} else if (s->wan_up > 0) {
		if (m->rx_bps >= 0) {
			fmt_rate(b2, sizeof(b2), m->rx_bps);
			snprintf(b, sizeof(b), S_DN "%s", b2);
			x = text(8, 221, b, LV(C_RX), 1) + 10;
			fmt_rate(b2, sizeof(b2), m->tx_bps);
			snprintf(b, sizeof(b), S_UP "%s", b2);
			text(x, 221, b, LV(C_TX), 1);
		} else {
			text(8, 221, "--", C_DIM, 1);
		}
		fmt_uptime(b, sizeof(b), s->wan_uptime);
		text_r(W - 8, 221, b, LV(C_TEXT), 1);
		text_r(W - 8 - text_w(b, 1) - 8, 221, "WAN", C_DIM, 1);
	} else {
		text(8, 221, "--", C_DIM, 1);
	}
#undef LV
}

static void render_page(const struct state *st)
{
	if (st->page)
		render_modem(st);
	else
		render(st);
}

#ifndef PREVIEW
int main(int argc, char **argv)
{
	static struct state st;
	const char *wan = argc > 1 ? argv[1] : "wan";
	unsigned long long ct0[MAXCPU + 1] = { 0 }, ci0[MAXCPU + 1] = { 0 }, rx0, tx0;
	static struct gest g;
	struct timespec t0, last_rx = { 0, 0 };
	time_t last_sec = 0, bat_since = 0, chg_since = 0, bat_min = 0;	/* с какого момента от батареи / на зарядке */
	double now_m, last_touch = 0;
	int fd, ev, touch_err = 0, bat_tries = 0, bat_fail = 0;

	st.wf[0] = (struct wifi){ .ifname = argc > 2 ? argv[2] : "phy1-ap0", .label = "2.4GHz" };
	st.wf[1] = (struct wifi){ .ifname = argc > 3 ? argv[3] : "phy0-ap0", .label = "5GHz" };
	st.wanw = (struct wifi){ .ifname = wan, .label = "WAN" };

	openlog("almond-lcd", LOG_PID, LOG_DAEMON);
	fd = open("/dev/lcd", O_WRONLY);
	if (fd < 0) {
		perror("/dev/lcd");
		return 1;
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGUSR1, on_usr1);
	theme_load();
	tcal_load();
	m2_init(&st.m2);
	touch_init();

	cpu_sample(ct0, ci0);
	rx0 = net_stat(wan, "rx_bytes");
	tx0 = net_stat(wan, "tx_bytes");
	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (!stop) {
		struct timespec rt, mono, sl;
		int i, full, redraw = 0, tp;

		/* тик 50 мс: опрос тача; экран - раз в секунду или после смены темы */
		clock_gettime(CLOCK_REALTIME, &rt);
		sl.tv_sec = 0;
		sl.tv_nsec = 50000000L - rt.tv_nsec % 50000000L;
		nanosleep(&sl, NULL);
		if (stop)
			break;
		clock_gettime(CLOCK_REALTIME, &rt);
		clock_gettime(CLOCK_MONOTONIC, &mono);

		/* жесты: тап или свайп - другая страница, удержание - тема */
		tp = touch_poll();
		if (tp < 0) {
			if (tfd >= 0 && ++touch_err >= 100) {
				syslog(LOG_WARNING, "touch: bus errors, re-init");
				touch_err = 0;
				touch_init();
			}
		} else {
			touch_err = 0;
		}
		now_m = mono.tv_sec + mono.tv_nsec / 1e9;
		ev = gesture_feed(&g, tp == 1, tp == 1 ? touch_x : -1, tp == 1 ? touch_y : -1, now_m);
		if (ev != G_NONE) {
			/* для калибровки: где было касание (сырые и экранные координаты) */
			syslog(LOG_INFO, "touch: gesture %d at %d,%d -> %d,%d raw %d,%d", ev,
			       g.x0, g.y0, g.x1, g.y1, raw_x, raw_y);
			last_touch = now_m;
			if (ev == G_LONG) {
				toggle_req = 1;
			} else if (st.s.has_modem) {
				st.page = !st.page;
				syslog(LOG_INFO, "page -> %s", st.page ? "modem" : "almond");
				redraw = 1;
			}
		}
		/* сброс после записи жеста, иначе в журнал попадают уже стёртые сырые значения */
		if (tp != 1 && !g.down && g.ticks == 0)
			touch_reset();
		/* со страницы модема - на главную через 5 минут без касаний */
		if (st.page && now_m - last_touch >= 300) {
			st.page = 0;
			syslog(LOG_INFO, "page -> almond (timeout)");
			redraw = 1;
		}
		if (toggle_req) {
			toggle_req = 0;
			theme_toggle();
			redraw = 1;
		}

		full = rt.tv_sec != last_sec;
		if (!full && !redraw)
			continue;
		st.phase = (int)(rt.tv_sec & 1);

		if (full) {
			unsigned long long ct[MAXCPU + 1] = { 0 }, ci[MAXCPU + 1] = { 0 }, rx, tx;
			double dt;
			char b[64];
			int n, k;

			last_sec = rt.tv_sec;
			dt = (mono.tv_sec - t0.tv_sec) + (mono.tv_nsec - t0.tv_nsec) / 1e9;
			t0 = mono;
			if (dt <= 0)
				dt = 1;
			n = cpu_sample(ct, ci);
			if (n >= 0) {
				if (ct[0] > ct0[0])
					st.cpu_pct = (int)(100 - (ci[0] - ci0[0]) * 100 / (ct[0] - ct0[0]));
				st.ncpu = n;
				for (k = 1; k <= n; k++)
					st.core_pct[k - 1] = ct[k] > ct0[k] ?
						(int)(100 - (ci[k] - ci0[k]) * 100 / (ct[k] - ct0[k])) : 0;
				memcpy(ct0, ct, sizeof(ct0));
				memcpy(ci0, ci, sizeof(ci0));
			}
			mem_sample(&st.mem_tot, &st.mem_av);
			rx = net_stat(wan, "rx_bytes");
			tx = net_stat(wan, "tx_bytes");
			st.rx = rx >= rx0 ? (rx - rx0) / dt : 0;
			st.tx = tx >= tx0 ? (tx - tx0) / dt : 0;
			rx0 = rx;
			tx0 = tx;
			for (i = 0; i < HIST - 1; i++) {
				st.rx_hist[i] = st.rx_hist[i + 1];
				st.tx_hist[i] = st.tx_hist[i + 1];
			}
			st.rx_hist[HIST - 1] = st.rx;
			st.tx_hist[HIST - 1] = st.tx;
			if (read_file("/proc/loadavg", b, sizeof(b)) > 0)
				snprintf(st.loadavg, sizeof(st.loadavg), "%.14s", b);
			if (read_file("/proc/uptime", b, sizeof(b)) > 0)
				st.uptime = strtod(b, NULL);
			st.sb_alive = proc_alive("sing-box", NULL);
			/* любой обратный SSH-туннель, порт и сервер не важны: ssh - это
			 * OpenSSH или ссылка на dropbear, dbclient - dropbear напрямую.
			 * Индикатор появляется, как только туннель был замечен. */
			{
				int tun_up = proc_alive("ssh", " -R") || proc_alive("dbclient", " -R");

				st.tun_cfg |= tun_up;
				st.tun_alive = tun_up;
			}
			slow_read(&st.s);
			st.slow_fresh = st.s.ts && time(NULL) - st.s.ts < 180;
			m2_update(&st.m2);

			/* активность Wi-Fi и WAN - за всю прошедшую секунду */
			wifi_sample(&st.wf[0], 1);
			wifi_sample(&st.wf[1], 1);
			wifi_sample(&st.wanw, 1);
			if (st.wanw.rx_act || !last_rx.tv_sec)
				last_rx = mono;
			st.rx_idle = (mono.tv_sec - last_rx.tv_sec) + (mono.tv_nsec - last_rx.tv_nsec) / 1e9;

			/* батарея раз в 15 с: два чтения подряд, статус берём, только если
			 * они совпали (первое после смены STAT несёт флаги перехода);
			 * три неудачи подряд - PIC считается молчащим */
			if (rt.tv_sec % 15 == 0 || !bat_tries) {
				int a1, s1, a2, s2;

				if (!bat_read(&a1, &s1) && !bat_read(&a2, &s2)) {
					bat_fail = 0;
					if (s1 == s2) {
						if (!st.bat_ok || s2 != st.bat_stat) {
							char msg[96];
							int k = snprintf(msg, sizeof(msg), "stat 0x%02x adc %d (%s)", s2, a2,
									 (s2 & BAT_NONE) == BAT_NONE ? "no battery" :
									 !(s2 & BAT_EXT) ? "on battery" :
									 (s2 & BAT_IDLE) ? "external power, charged" :
									 "external power, charging");
							long d = bat_since ? (long)(time(NULL) - bat_since) : 0;

							if (on_battery(&st) && (s2 & BAT_EXT) && bat_since)
								snprintf(msg + k, sizeof(msg) - k, " after %ldm%02lds on battery",
									 d / 60, d % 60);
							else if (charging(&st) && (s2 & BAT_IDLE) && chg_since)
								snprintf(msg + k, sizeof(msg) - k, " after %ldm charging",
									 (long)(time(NULL) - chg_since) / 60);
							syslog(LOG_INFO, "battery: %s", msg);
							bat_log(msg);
						}
						st.bat_ok = 1;
						st.bat_adc = a2;
						st.bat_stat = s2;
						if (!on_battery(&st))
							bat_since = 0;
						else if (!bat_since)
							bat_since = time(NULL);
						if (!charging(&st))
							chg_since = 0;
						else if (!chg_since)
							chg_since = time(NULL);
						/* от батареи и на зарядке - раз в минуту АЦП: кривые разряда
						 * и заряда для калибровки */
						if ((bat_since || chg_since) && rt.tv_sec / 60 != bat_min) {
							char msg[64];
							long d = (long)(time(NULL) - (bat_since ? bat_since : chg_since));

							bat_min = rt.tv_sec / 60;
							snprintf(msg, sizeof(msg), "%s %ldm%02lds adc %d",
								 bat_since ? "on battery" : "charging", d / 60, d % 60, a2);
							bat_log(msg);
						}
					}
				} else if (++bat_fail >= 3 && st.bat_ok) {
					st.bat_ok = 0;
					syslog(LOG_WARNING, "battery: PIC at 0x2A does not answer");
					bat_log("PIC at 0x2A does not answer");
				}
				bat_tries++;
			}
		}

		render_page(&st);
		if (flush(fd) < 0) {
			perror("write /dev/lcd");
			sleep(5);
			shown_valid = 0;
		}
	}
	close(fd);
	return 0;
}
#endif /* PREVIEW */
